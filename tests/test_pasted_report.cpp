// A report pasted into the bug form is recognised (0.99.69): core/pasted_report.hpp.
//
// WHAT IS PINNED HERE
//   - a GENUINE sentinel report, the one finishSentinelWatch writes for an ending
//     from outside (the same in-process seam tests/test_sentinel.cpp drives), is
//     recognised as `sentinel:outside` with the version that wrote it; and a report
//     rendered for every one of the sentinel's five classes names the class the
//     sentinel's own table names - so the wire vocabulary cannot drift from it;
//   - a crash report, a freeze report and a display-stall report, in the shapes the
//     two writers produce (the fixtures of tests/test_crash_upload.cpp), and that
//     the real parser (parseReportText) reads what the detector read;
//   - what is NOT a report: prose, prose that mentions the word "sentinel" or quotes
//     a `kind: crash` line, and every half-report (no signature, no context block,
//     no version, an unknown kind);
//   - line endings: the same report with CRLF and with LF is recognised alike, and
//     what travels is LF;
//   - the slice: prose before the report is not part of it, a complete report after
//     an incomplete quote of one is the one found, an oversize paste is cut at a line;
//   - the wire: the body core/problem_report.hpp builds for a genuine sentinel report
//     pasted into a message has `attachments` with exactly the three documented keys, an
//     old client's body has none, and the body can be written out as the site's fixture.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/breadcrumb.hpp"
#include "core/crash_upload.hpp"
#include "core/pasted_report.hpp"
#include "core/problem_report.hpp"
#include "core/sentinel.hpp"
#include "core/version.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir = base / (std::string("cascade-pasted-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::string toCrlf(const std::string& lf) {
    std::string out;
    for (const char c : lf) {
        if (c == '\n') { out += '\r'; }
        out += c;
    }
    return out;
}

// The same fixtures tests/test_crash_upload.cpp and tests/test_diag_history.cpp use for
// the two in-process writers' shapes (crash_handler.cpp and hang_watchdog.cpp).
std::string crashReport() {
    return "kind: crash\n"
           "reason: access violation\n"
           "code: 0xC0000005\n"
           "address: cascade.exe+0x1A2B\n"
           "signature: 0123456789ABCDEF\n"
           "thread: 24180\n"
           "--- context ---\n"
           "version: 0.62.0\n"
           "commit: abc123def456\n"
           "os: Windows 10.0.22631\n"
           "arch: x64\n"
           "--- stack (thread 24180) ---\n"
           "  cascade.exe+0x1A2B\n"
           "--- process ---\n"
           "uptime-sec: 45\n"
           "fault-thread-own: no\n"
           "--- log (last 2 of 4011 lines) ---\n"
           "source opened\n"
           "plugin started\n";
}

std::string freezeReport(const char* kind) {
    return std::string("kind: ") + kind + "\n" +
           "note: the gui thread did not complete a frame within the threshold\n"
           "stalled-ms: 7213\n"
           "threshold-ms: 5000\n"
           "signature: FEDCBA9876543210\n"
           "threads: 2\n"
           "frame-scope: rail\n"
           "--- context ---\n"
           "version: 0.99.59\n"
           "commit: abc123\n"
           "--- process ---\n"
           "uptime-sec: 1200\n"
           "--- thread 1 (gui, stalled) ---\n"
           "  ntdll.dll+0x1234\n";
}

// A sentinel report as the application's own renderer writes it, for a class.
std::string renderedSentinel(SentinelClass c) {
    SentinelVerdict v;
    v.cls = c;
    v.reason = std::string(sentinelClassReason(c)) + " - ended by another process (exit code 1, as taskkill /F does); phase running";
    v.code = 1;
    v.codeKnown = true;
    v.signatureTag = std::string("sentinel:") + sentinelClassId(c) + ":running";
    SentinelReportInfo info;
    info.version = "0.99.66";
    info.commit = "0123456789ab";
    info.os = "Windows 10.0.22631";
    info.arch = "x64";
    info.uptimeSec = 1234;
    info.logLines = {"12:00:00.000 info FoxSDR 0.99.66 (0123456789ab) starting", "12:00:01.000 info the last line"};
    info.logTotalLines = 2;
    return renderSentinelReport(v, info);
}

}  // namespace

int main() {
    // =======================================================================
    // 1. A GENUINE SENTINEL REPORT, from the real seam: an ending from outside
    // =======================================================================
    {
        const fs::path dir = scratchDir("genuine");
        const fs::path logs = scratchDir("genuinelogs");
        writeFile(logs / "foxsdr.log",
                  "12:00:00.000 info FoxSDR 0.99.66 (abc123def456) starting\n"
                  "12:00:01.000 info source: opened\n12:00:02.000 info the last line\n");
        SentinelEnd e;
        e.appPid = 7101;
        e.exitKnown = true;
        e.exitCode = 1;  // what taskkill /F leaves
        breadcrumb::Snapshot s;
        s.valid = true;
        const std::uint64_t now = breadcrumb::nowMs();
        s.startedMs = now - 60000;
        s.phase = breadcrumb::Phase::Running;
        s.frames = 600;
        s.beatMs = now - 10;
        s.phaseMs = now - 50000;
        e.crumb = s;
        e.crashDir = dir.string();
        e.logDir = logs.string();
        e.uptimeSec = 77;
        const SentinelOutcome o = finishSentinelWatch(e);
        CHECK(o.verdict.cls == SentinelClass::Outside);
        CHECK(!o.reportPath.empty());
        const std::string onDisk = readFile(o.reportPath);
        CHECK(contains(onDisk, "reason: sentinel: ended from outside, window was drawing"));

        const std::optional<PastedReport> p = detectPastedReport(onDisk);
        CHECK(p.has_value());
        if (p) {
            CHECK(p->reportClass == "sentinel:outside");
            CHECK(p->version == cascade::versionString());
            CHECK(p->text == onDisk);  // the whole file, byte for byte (it is LF already)
        }
        // The same file as a person pastes it: a sentence of their own first, and
        // Windows line endings.
        const std::optional<PastedReport> viaPaste =
            detectPastedReport("Hello, FoxSDR says upload local-only. Here is the report:\r\n\r\n" + toCrlf(onDisk));
        CHECK(viaPaste.has_value());
        if (viaPaste && p) {
            CHECK(viaPaste->reportClass == p->reportClass);
            CHECK(viaPaste->version == p->version);
            CHECK(viaPaste->text == p->text);
            CHECK(!contains(viaPaste->text, "\r"));
            CHECK(!contains(viaPaste->text, "Hello"));
        }
        // And the real parser reads that slice the way the detector did.
        ParsedReport parsed;
        CHECK(p.has_value() && parseReportText(p->text, parsed));
        if (p) {
            CHECK(parsed.kind == "crash");
            CHECK(parsed.version == p->version);
            CHECK(sentinelReasonIsLocalOnly(parsed.reason));
        }
        std::error_code ec;
        if (g_checksFailed == 0) {
            fs::remove_all(dir, ec);
            fs::remove_all(logs, ec);
        }
    }

    // =======================================================================
    // 2. EVERY SENTINEL CLASS names the class the sentinel's own table names
    // =======================================================================
    {
        for (const SentinelClass c : {SentinelClass::Crash, SentinelClass::Frozen, SentinelClass::Startup,
                                      SentinelClass::Outside, SentinelClass::Session}) {
            const std::string text = renderedSentinel(c);
            const std::optional<PastedReport> p = detectPastedReport(text);
            CHECK(p.has_value());
            if (p) {
                CHECK(p->reportClass == std::string("sentinel:") + sentinelClassId(c));
                CHECK(p->version == "0.99.66");
            }
        }
        // The closed list is exactly these, then the three kinds.
        const std::vector<std::string> want = {"crash",           "hang",
                                               "stall",           "sentinel:crash",
                                               "sentinel:frozen", "sentinel:startup",
                                               "sentinel:outside", "sentinel:session"};
        CHECK(pastedReportClassIds() == want);
    }

    // =======================================================================
    // 3. THE OTHER REPORTS: crash, freeze, display stall
    // =======================================================================
    {
        const std::optional<PastedReport> c = detectPastedReport(crashReport());
        CHECK(c.has_value() && c->reportClass == "crash" && c->version == "0.62.0");
        const std::optional<PastedReport> h = detectPastedReport(freezeReport("hang"));
        CHECK(h.has_value() && h->reportClass == "hang" && h->version == "0.99.59");
        const std::optional<PastedReport> st = detectPastedReport(freezeReport("stall"));
        CHECK(st.has_value() && st->reportClass == "stall" && st->version == "0.99.59");

        // The real parser agrees with what was read, for each.
        for (const std::string& t : {crashReport(), freezeReport("hang"), freezeReport("stall")}) {
            const std::optional<PastedReport> p = detectPastedReport(t);
            ParsedReport r;
            CHECK(p.has_value() && parseReportText(p->text, r));
            if (p) {
                CHECK(r.version == p->version);
                CHECK(r.kind == p->reportClass);
                CHECK(r.signature == "0123456789ABCDEF" || r.signature == "FEDCBA9876543210");
            }
        }

        // An ordinary crash whose reason merely contains the word is still a crash.
        std::string odd = crashReport();
        odd.replace(odd.find("access violation"), 16, "the sentinel was not involved");
        const std::optional<PastedReport> o = detectPastedReport(odd);
        CHECK(o.has_value() && o->reportClass == "crash");
        // A sentinel-looking reason that is not one of the five sentences is a crash.
        std::string future = crashReport();
        future.replace(future.find("access violation"), 16, "sentinel: a class a later build adds");
        const std::optional<PastedReport> f = detectPastedReport(future);
        CHECK(f.has_value() && f->reportClass == "crash");
    }

    // =======================================================================
    // 4. WHAT IS NOT A REPORT
    // =======================================================================
    {
        CHECK(!detectPastedReport("").has_value());
        CHECK(!detectPastedReport("The waterfall freezes when I change the sample rate.").has_value());
        // Prose that merely mentions the word, and the words of the status line.
        CHECK(!detectPastedReport("The sentinel says my report is local-only. Why does it say upload local-only?")
                   .has_value());
        CHECK(!detectPastedReport("it printed: sentinel: ended from outside, window was drawing and then nothing")
                   .has_value());
        // A quoted kind line alone, and in every half-report.
        CHECK(!detectPastedReport("kind: crash\n").has_value());
        CHECK(!detectPastedReport("I saw this:\nkind: crash\nand then it closed\n").has_value());
        // No signature.
        CHECK(!detectPastedReport("kind: crash\nreason: access violation\n--- context ---\nversion: 0.99.66\n").has_value());
        // No context block.
        CHECK(!detectPastedReport("kind: crash\nreason: access violation\nsignature: 0123456789ABCDEF\n").has_value());
        // A context block with no version.
        CHECK(!detectPastedReport("kind: crash\nsignature: 0123456789ABCDEF\n--- context ---\ncommit: abc\n").has_value());
        // A signature that is not hex.
        CHECK(!detectPastedReport("kind: crash\nsignature: not-a-signature\n--- context ---\nversion: 0.99.66\n").has_value());
        // An unknown kind.
        CHECK(!detectPastedReport("kind: banana\nsignature: 0123456789ABCDEF\n--- context ---\nversion: 0.99.66\n").has_value());
        // The pieces in the wrong order: the context marker before the header ends.
        CHECK(!detectPastedReport("kind: crash\n--- context ---\nversion: 0.99.66\nsignature: 0123456789ABCDEF\n").has_value());
        // A version that is not made of version characters is no version, and the
        // report is not recognised on its strength.
        CHECK(!detectPastedReport("kind: crash\nsignature: 0123456789ABCDEF\n--- context ---\nversion: <script>\n").has_value());
    }

    // =======================================================================
    // 5. LINE ENDINGS: CRLF and LF are the same report
    // =======================================================================
    {
        const std::string lf = renderedSentinel(SentinelClass::Outside);
        const std::string crlf = toCrlf(lf);
        CHECK(contains(crlf, "\r\n"));
        const std::optional<PastedReport> a = detectPastedReport(lf);
        const std::optional<PastedReport> b = detectPastedReport(crlf);
        CHECK(a.has_value() && b.has_value());
        if (a && b) {
            CHECK(a->reportClass == b->reportClass && a->version == b->version && a->text == b->text);
            CHECK(!contains(b->text, "\r"));
            CHECK(a->text == lf);
        }
        // A lone CR (an old paste buffer) too.
        std::string cr = lf;
        for (char& c : cr) {
            if (c == '\n') { c = '\r'; }
        }
        const std::optional<PastedReport> d = detectPastedReport(cr);
        CHECK(d.has_value() && a.has_value() && d->text == a->text);
    }

    // =======================================================================
    // 6. THE SLICE
    // =======================================================================
    {
        // Prose before the report is not part of it.
        const std::string rep = renderedSentinel(SentinelClass::Crash);
        const std::optional<PastedReport> p = detectPastedReport("Please look at this.\n\n" + rep);
        CHECK(p.has_value() && p->text == rep);

        // An incomplete quote first, a complete report after it: the complete one.
        const std::optional<PastedReport> q =
            detectPastedReport("it started with\nkind: crash\nthen\n\n" + rep);
        CHECK(q.has_value() && q->text == rep);

        // The first complete report of two.
        const std::optional<PastedReport> two = detectPastedReport(crashReport() + "\n" + rep);
        CHECK(two.has_value() && two->reportClass == "crash");

        // Indentation on the kind line (a paste out of an indented mail) is allowed.
        const std::optional<PastedReport> ind = detectPastedReport("  kind: crash\n" + rep.substr(rep.find('\n') + 1));
        CHECK(ind.has_value() && ind->reportClass == "sentinel:crash");
    }

    // =======================================================================
    // 7. AN OVERSIZE PASTE is cut at a line, and says so
    // =======================================================================
    {
        std::string big = crashReport();
        for (int i = 0; i < 4000; ++i) { big += "12:00:00.000 info a long log line to make the paste larger than the cap " + std::to_string(i) + "\n"; }
        CHECK(big.size() > kPastedReportMaxBytes);
        const std::optional<PastedReport> p = detectPastedReport(big);
        CHECK(p.has_value());
        if (p) {
            CHECK(p->text.size() <= kPastedReportMaxBytes);
            CHECK(p->text.size() > kPastedReportMaxBytes - 512);
            CHECK(p->text.back() == '\n');
            CHECK(contains(p->text, "(cut here: the rest of the report is in the message)\n"));
            CHECK(p->text.rfind("kind: crash\n", 0) == 0);
            // Cut at a line: the line before the note is whole.
            const std::size_t note = p->text.find("(cut here:");
            CHECK(note != std::string::npos && p->text[note - 1] == '\n');
        }
        // Exactly at the cap is not cut.
        std::string exact = crashReport();
        while (exact.size() + 10 < kPastedReportMaxBytes) { exact += "0123456789\n"; }
        const std::optional<PastedReport> e = detectPastedReport(exact);
        CHECK(e.has_value() && !contains(e->text, "(cut here:") && e->text.size() <= kPastedReportMaxBytes);
    }

    // =======================================================================
    // 8. THE WIRE: a genuine sentinel report pasted into a bug report, as the
    //    page sends it (core/problem_report.hpp), and read back
    // =======================================================================
    //
    // THE FIXTURE THE SITE'S TEST READS. With FOXSDR_PROBLEM_FIXTURE_OUT set to a
    // path, the body this block builds is written there, byte for byte; the site's
    // repository keeps a copy as testdata/problem_report_pasted_sentinel.json and its
    // Go test (problems_attachments_test.go) posts it to the real handler and checks
    // what the pages say. Regenerate it with
    //     set FOXSDR_PROBLEM_FIXTURE_OUT=<site>\testdata\problem_report_pasted_sentinel.json
    //     ctest -C Release -R test_pasted_report
    // whenever the shape of `attachments` changes, so the two repositories cannot
    // quietly disagree about it.
    {
        const fs::path dir = scratchDir("wire");
        const fs::path logs = scratchDir("wirelogs");
        writeFile(logs / "foxsdr.log",
                  "12:00:00.000 info FoxSDR 0.99.66 (abc123def456) starting\n"
                  "12:00:01.000 info source: opened\n12:00:02.000 info the last line\n");
        SentinelEnd e;
        e.appPid = 7102;
        e.exitKnown = true;
        e.exitCode = 1;
        breadcrumb::Snapshot s;
        s.valid = true;
        const std::uint64_t now = breadcrumb::nowMs();
        s.startedMs = now - 60000;
        s.phase = breadcrumb::Phase::Running;
        s.frames = 600;
        s.beatMs = now - 10;
        s.phaseMs = now - 50000;
        e.crumb = s;
        e.crashDir = dir.string();
        e.logDir = logs.string();
        e.uptimeSec = 77;
        const SentinelOutcome o = finishSentinelWatch(e);
        const std::string report = readFile(o.reportPath);

        ProblemReportPayload p;
        p.kind = kProblemKindBug;
        p.text = "Hello. The bundle said upload local-only and I do not know what that means, "
                 "so here is the report it listed:\r\n\r\n" + toCrlf(report);
        p.contact = "";
        p.version = "0.99.66";
        p.platform = "windows";
        p.arch = "x64";
        const std::optional<PastedReport> found = detectPastedReport(p.text);
        CHECK(found.has_value());
        if (found) { p.attachments.push_back(*found); }
        const std::string body = problemReportJson(p);

        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        CHECK(j.is_object() && j.contains("attachments") && j["attachments"].is_array() &&
              j["attachments"].size() == 1);
        if (j.is_object() && j.contains("attachments") && j["attachments"].size() == 1) {
            const nlohmann::json& a = j["attachments"][0];
            std::vector<std::string> keys;
            for (auto it = a.begin(); it != a.end(); ++it) { keys.push_back(it.key()); }
            // nlohmann orders keys; the inventory is a set.
            std::vector<std::string> want = problemReportAttachmentFieldNames();
            std::sort(want.begin(), want.end());
            CHECK(keys == want);
            CHECK(a.value("class", std::string()) == "sentinel:outside");
            CHECK(a.value("version", std::string()) == cascade::versionString());
            CHECK(a.value("text", std::string()) == report);  // LF, whatever the paste was
            // And the message text still carries the same words, unchanged but for its
            // trim: nothing is sent that was not already in the message.
            CHECK(j.value("text", std::string()).find("kind: crash") != std::string::npos);
        }
        // Old clients stay valid: the same payload without the attachment has no key.
        ProblemReportPayload plain = p;
        plain.attachments.clear();
        CHECK(problemReportJson(plain).find("attachments") == std::string::npos);

        if (const char* out = std::getenv("FOXSDR_PROBLEM_FIXTURE_OUT")) {
            if (*out != '\0') { writeFile(fs::path(out), body + "\n"); }
        }
        std::error_code ec;
        if (g_checksFailed == 0) {
            fs::remove_all(dir, ec);
            fs::remove_all(logs, ec);
        }
    }

    return testSummary("test_pasted_report");
}
