// THE BUNDLE REACHES BACK PAST THE CURRENT SESSION: the end of the previous
// session's log, and one line per crash or freeze report on the machine.
//
// THE FIELD CASE (0.99.59). A window froze after twenty minutes; the user ended
// it from the taskbar, restarted, and sent the bundle five minutes into the next
// session. The bundle described a healthy five-minute session - the log ring is in
// memory and holds the CURRENT session only - and nothing in it said whether a
// freeze report had been written or uploaded. The session that froze was gone.
//
// WHAT THIS HOLDS, against scratch folders and never the machine's own:
//   - the previous session is found across the rotating files (it began in
//     foxsdr.1.log and the current one is in foxsdr.log), cut at the
//     `FoxSDR <version> (<commit>) starting` line, bounded by lines AND bytes with
//     the NEWEST lines kept, and the bundle says plainly when there is none;
//   - the report list names the newest ten crash-* / hang-* files with their
//     kind, age, version, uptime, stalled-ms or reason and code, signature and
//     upload status (none included), and carries no stack, no log, no path;
//   - every line of both comes out of the same scrub as the current log: a
//     frequency, a serial number, a user's name in a path and an address;
//   - with diagnostics off nothing is read - not a file opened, not a worker
//     started, not a heading written - through the real AppWindow;
//   - the sections survive the problem report's 64 KiB cut with a full ring.
//
// Not in the automatic crash upload: that reads a fixed field list
// (core/crash_upload.cpp), which this does not touch; test_crash_upload holds it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_history.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/problem_report.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_diag_history_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

void setAge(const fs::path& p, int seconds) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(seconds), ec);
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

// The text of a bundle after `heading`, up to the next heading or the end.
std::string sectionOf(const std::string& bundle, const char* heading) {
    const std::size_t at = bundle.find(heading);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t from = at + std::string(heading).size();
    const std::size_t next = bundle.find("\n--- ", from);
    return bundle.substr(from, next == std::string::npos ? std::string::npos : next - from);
}

std::string line(int minute, int second, const std::string& text) {
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "12:%02d:%02d.000", minute, second);
    return std::string(stamp) + " info " + text + "\n";
}

// --- the scratch log folder: two sessions and the start of a third -----------
//
//   foxsdr.2.log   session A (an old one)
//   foxsdr.1.log   the tail of A, then session B - the PREVIOUS session - whole
//   foxsdr.log     session C, the current one
//
// B has more lines than the bound, and three that must not leave the machine
// as they are written: a frequency, a serial number, a user's name in a path
// (and an address).
constexpr int kBLines = 120;

void seedLogs(const fs::path& logDir) {
    std::string a2 = line(0, 0, "FoxSDR 0.99.50 (aaaaaaaaaaaa) starting");
    for (int i = 0; i < 3; ++i) { a2 += line(0, 1 + i, "source: session A line " + std::to_string(i)); }
    writeFile(logDir / "foxsdr.2.log", a2);

    std::string b = line(1, 0, "source: session A's last line, in the next file");
    b += line(2, 0, "FoxSDR 0.99.59 (b0b0b0b0b0b0) starting");
    for (int i = 0; i < kBLines; ++i) {
        std::string text = "mode: session B line " + std::to_string(i);
        if (i == 100) { text = "source: asked for 433.917000 MHz"; }
        if (i == 101) { text = "source: opened RTL2838UHIDIR serial 00000001 (rtlsdr) at 2048000 S/s"; }
        if (i == 102) { text = "config: C:\\Users\\Alice\\AppData\\Local\\FoxSDR\\logs\\foxsdr.log"; }
        if (i == 103) { text = "source: connecting to 192.0.2.20:1234"; }
        if (i == kBLines - 1) { text = "audio: session B's LAST line before the freeze"; }
        b += line(3 + i / 50, i % 50, text);
    }
    writeFile(logDir / "foxsdr.1.log", b);

    std::string c = line(10, 0, "FoxSDR 0.99.61 (c0c0c0c0c0c0) starting");
    for (int i = 0; i < 5; ++i) { c += line(10, 1 + i, "source: session C line " + std::to_string(i)); }
    writeFile(logDir / "foxsdr.log", c);
}

// --- the scratch reports folder ----------------------------------------------
std::string crashReport() {
    return "kind: crash\n"
           "reason: access violation\n"
           "code: 0xC0000005\n"
           "address: cascade.exe+0x1A2B\n"
           "signature: 1B2C3D4E5F607182\n"
           "thread: 1234\n"
           "--- context ---\n"
           "version: 0.99.59\n"
           "commit: abc123\n"
           "os: Windows 10.0.19045\n"
           "mode: WFM\n"
           "source: soapy\n"
           "sdr-model: SECRET-MODEL\n"
           "--- stack (thread 1234) ---\n"
           "  cascade.exe+0x1A2B SECRET-STACK-FRAME\n"
           "--- process ---\n"
           "uptime-sec: 3600\n"
           "fault-thread-own: yes\n"
           "--- modules ---\n"
           "  cascade.exe base=0x00007FF600000000 size=0x100 pdb=cascade.pdb build=ABCD\n"
           "--- log (last 1 of 1 lines) ---\n"
           "12:00:00.000 info SECRET-LOG-LINE C:\\Users\\bob\\Documents\\secret.iq\n";
}

std::string hangReport(const char* kind, int stalledMs, const char* sig, int uptime) {
    return std::string("kind: ") + kind + "\n" +
           "note: the gui thread did not complete a frame within the threshold\n"
           "stalled-ms: " + std::to_string(stalledMs) + "\n" +
           "threshold-ms: 5000\n"
           "signature: " + sig + "\n" +
           "threads: 9\n"
           "--- context ---\n"
           "version: 0.99.59\n"
           "commit: abc123\n"
           "--- modules ---\n"
           "  cascade.exe base=0x00007FF600000000 size=0x100 pdb=cascade.pdb build=ABCD\n"
           "--- process ---\n"
           "uptime-sec: " + std::to_string(uptime) + "\n" +
           "--- thread 1 (gui, stalled) ---\n"
           "  ntdll.dll+0x1234 SECRET-STACK-FRAME\n";
}

void sidecar(const fs::path& report, const std::string& status) {
    writeFile(fs::path(report.string() + ".upload"),
              "status: " + status + "\nattempts: 1\nat: 2026-10-04 10:00:00\nsignature: x\nnote: n\n");
}

void seedReports(const fs::path& crashDir) {
    const fs::path crash = crashDir / "crash-20260930-101010-123-1.txt";
    writeFile(crash, crashReport());
    setAge(crash, 7200);  // no sidecar: nothing has swept it

    const fs::path hang = crashDir / "hang-456-1.txt";
    writeFile(hang, hangReport("hang", 7213, "7C04AAAABBBBCCCC", 1200));
    sidecar(hang, "sent");
    setAge(hang, 90);

    const fs::path stall = crashDir / "hang-456-2.txt";
    writeFile(stall, hangReport("stall", 5400, "5A11AAAABBBBCCCC", 1210));
    sidecar(stall, "local-only");
    setAge(stall, 20);

    // Things in the folder that are NOT reports and must never be listed or read:
    // a memory dump, the bundle the user copied for themselves, a stray note, and
    // a sidecar by itself.
    writeFile(crashDir / "crash-20260930-101010-123-1.dmp", "MEMORY-DUMP-BYTES SECRET-DUMP");
    writeFile(crashDir / "diagnostics.txt", "kind: crash\nsignature: DEADBEEFDEADBEEF\nSECRET-BUNDLE\n");
    writeFile(crashDir / "notes.txt", "kind: crash\nsignature: FEEDFACEFEEDFACE\n");
}

// A bundle with the sections, built the way the application builds one.
std::string bundleWith(const DiagHistory& h, std::vector<std::string> ring = {}) {
    DiagBundleInput in;
    in.context.version = "0.99.61";
    in.context.commit = "c0c0c0c0c0c0";
    in.logLines = std::move(ring);
    if (in.logLines.empty()) { in.logLines.push_back("12:10:05.000 info current session line"); }
    in.logPath = "C:/Users/x/AppData/Local/FoxSDR/logs/foxsdr.log";
    in.crashDir = "C:/Users/x/AppData/Local/FoxSDR/crashes";
    in.history = h;
    return buildDiagnosticsBundle(in);
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static std::string bundle(AppWindow& a, bool fresh) { return a.currentDiagnosticsBundle(fresh); }
    static void apply(AppWindow& a, bool on) { a.applyDiagnosticsEnabled(on); }
    static std::size_t reads(const AppWindow& a) { return a.diagHistory_.readsStarted(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

void testPreviousSession() {
    std::printf("the previous session's log\n");
    const fs::path logDir = g_scratch / "case1" / "logs";
    seedLogs(logDir);

    // Session boundaries are the start line, in a log file's own format.
    CHECK(isSessionStartLine("12:00:00.000 info FoxSDR 0.99.61 (abc123def456) starting"));
    CHECK(isSessionStartLine("12:00:00.000 info FoxSDR 0.99.61-nightly.1 (abc123def456-dirty) starting"));
    CHECK(!isSessionStartLine("12:00:00.000 info frame loop starting (interactive)"));
    CHECK(!isSessionStartLine("12:00:00.000 info FoxSDR is starting"));
    CHECK(!isSessionStartLine("12:00:00.000 info source: FoxSDR 0.99.61 (x) starting up"));

    const PreviousSessionLog p = readPreviousSessionLog(logDir.string());
    CHECK(p.found);
    CHECK(p.build == "FoxSDR 0.99.59 (b0b0b0b0b0b0)");
    // Session B: its start line and kBLines lines (A's last line is A's, not B's).
    CHECK(p.sessionLines == static_cast<std::size_t>(kBLines) + 1u);
    // BOUNDED BY LINES, and it is the NEWEST ones that are kept.
    CHECK(p.lines.size() == kPreviousSessionMaxLines);
    CHECK(has(p.lines.back(), "session B's LAST line before the freeze"));
    CHECK(has(p.lines.front(), "session B line " + std::to_string(kBLines - static_cast<int>(kPreviousSessionMaxLines))));
    // None of A, none of C.
    for (const std::string& l : p.lines) {
        CHECK(!has(l, "session A") && !has(l, "session C") && !has(l, "starting"));
    }

    // BOUNDED BY BYTES as well: the same files, a ceiling of 400 bytes.
    const PreviousSessionLog small = readPreviousSessionLog(logDir.string(), 80, 400);
    std::size_t bytes = 0;
    for (const std::string& l : small.lines) { bytes += l.size() + 1; }
    CHECK(small.found);
    CHECK(!small.lines.empty() && small.lines.size() < kPreviousSessionMaxLines);
    CHECK(bytes <= 400);
    CHECK(has(small.lines.back(), "LAST line"));

    // THE "NONE" WORDINGS, each different, each plain.
    {
        const fs::path only = g_scratch / "case1b" / "logs";
        writeFile(only / "foxsdr.log", line(0, 0, "FoxSDR 0.99.61 (c0c0c0c0c0c0) starting") +
                                           line(0, 1, "source: current"));
        const PreviousSessionLog n = readPreviousSessionLog(only.string());
        CHECK(!n.found && n.lines.empty());
        CHECK(has(n.reason, "no session before this one"));

        const fs::path nothing = g_scratch / "case1c" / "logs";
        writeFile(nothing / "foxsdr.log", line(0, 0, "source: no start line at all"));
        CHECK(has(readPreviousSessionLog(nothing.string()).reason, "no session start line"));

        fs::create_directories(g_scratch / "case1d" / "logs");
        CHECK(has(readPreviousSessionLog((g_scratch / "case1d" / "logs").string()).reason,
                  "holds no log file"));
        CHECK(has(readPreviousSessionLog((g_scratch / "case1e" / "logs").string()).reason,
                  "does not exist"));
        CHECK(!readPreviousSessionLog(std::string()).found);
    }

    // THE BUNDLE: its own heading, after the current log, every line scrubbed.
    {
        DiagHistory h;
        h.included = true;
        h.previous = p;
        const std::string b = bundleWith(h);
        const std::size_t logAt = b.find("--- log ---");
        const std::size_t prevAt = b.find(kPreviousSessionHeading);
        CHECK(logAt != std::string::npos && prevAt != std::string::npos && prevAt > logAt);
        const std::string sec = sectionOf(b, kPreviousSessionHeading);
        CHECK(has(sec, "session: FoxSDR 0.99.59 (b0b0b0b0b0b0), 121 lines in the log files, the last 80 follow"));
        CHECK(has(sec, "session B's LAST line before the freeze"));
        CHECK(!has(sec, "session C"));

        // THE SCRUB. Lines 100-103 of B are the last 20 of its 120 and are in the
        // window; each comes out of the same scrub as the current log.
        CHECK(!has(b, "433.917"));
        CHECK(has(sec, "asked for #"));
        CHECK(!has(b, "00000001"));
        CHECK(has(sec, "serial <stripped>"));
        CHECK(!has(b, "Alice"));
        CHECK(has(sec, "<user>"));
        CHECK(!has(b, "192.0.2.20"));
        CHECK(has(sec, "<host>:1234"));
    }

    // PENDING says so rather than claiming there is nothing.
    {
        DiagHistory h;
        h.included = true;
        h.pending = true;
        const std::string b = bundleWith(h);
        CHECK(has(sectionOf(b, kPreviousSessionHeading), "still being read"));
        CHECK(has(sectionOf(b, kReportsHeading), "still being read"));
    }
    // ...and the none wording lands in the bundle.
    {
        DiagHistory h;
        h.included = true;
        h.previous.reason = "the log files hold no session before this one";
        const std::string sec = sectionOf(bundleWith(h), kPreviousSessionHeading);
        CHECK(has(sec, "none - the log files hold no session before this one"));
    }
}

void testReports() {
    std::printf("the reports on this machine\n");
    const fs::path crashDir = g_scratch / "case2" / "crashes";
    seedReports(crashDir);
    const std::time_t now = std::time(nullptr);

    const ReportListing l = listRecentReports(crashDir.string(), now);
    CHECK(l.readable);
    CHECK(l.total == 3u);
    CHECK(l.newest.size() == 3u);
    // Newest first: the stall (20 s), the freeze (90 s), the crash (2 h).
    CHECK(l.newest.size() == 3u && l.newest[0].kind == "stall");
    CHECK(l.newest.size() == 3u && l.newest[1].kind == "hang");
    CHECK(l.newest.size() == 3u && l.newest[2].kind == "crash");

    const std::string sec = sectionOf(bundleWith([&] {
                                          DiagHistory h;
                                          h.included = true;
                                          h.reports = l;
                                          return h;
                                      }()),
                                      kReportsHeading);
    std::printf("%s\n", sec.c_str());
    CHECK(has(sec, "newest 3 of 3"));
    // THE FOUR THINGS THE FIELD CASE COULD NOT ANSWER, one line each.
    CHECK(has(sec, "stall, just now, version 0.99.59, uptime 1210 s, stalled 5400 ms, "
                   "signature 5A11AAAABBBBCCCC, upload local-only\n"));
    CHECK(has(sec, "hang, 1 min ago, version 0.99.59, uptime 1200 s, stalled 7213 ms, "
                   "signature 7C04AAAABBBBCCCC, upload sent\n"));
    CHECK(has(sec, "crash, 2 h ago, version 0.99.59, uptime 3600 s, access violation (0xC0000005), "
                   "signature 1B2C3D4E5F607182, upload none\n"));
    // Newest first in the text too.
    CHECK(sec.find("stall,") < sec.find("hang,") && sec.find("hang,") < sec.find("crash,"));

    // NO STACK, NO LOG, NO PATH, no file name, nothing from the context block, and
    // nothing from the files that are not reports.
    for (const char* secret : {"SECRET-STACK-FRAME", "SECRET-LOG-LINE", "SECRET-MODEL", "SECRET-DUMP",
                               "SECRET-BUNDLE", "bob", "secret.iq", "DEADBEEF", "FEEDFACE",
                               ".txt", ".dmp", "crash-2026", "hang-456", "\\", "cascade.exe"}) {
        if (has(sec, secret)) { std::printf("  leaked: %s\n", secret); }
        CHECK(!has(sec, secret));
    }

    // THE BOUND: twelve reports list ten, and say so.
    {
        const fs::path many = g_scratch / "case2b" / "crashes";
        for (int i = 0; i < 12; ++i) {
            const fs::path p = many / ("hang-1-" + std::to_string(i) + ".txt");
            writeFile(p, hangReport("hang", 6000 + i, "AAAABBBBCCCCDDDD", 100 + i));
            setAge(p, 1000 + i * 100);
        }
        const ReportListing m = listRecentReports(many.string(), now);
        CHECK(m.total == 12u && m.newest.size() == kListedReports);
        // The newest ten, not an arbitrary ten: ages 1000 .. 1900 s.
        CHECK(m.newest.size() == kListedReports && m.newest.front().stalledMs == 6000);
        CHECK(m.newest.size() == kListedReports && m.newest.back().stalledMs == 6009);
        DiagHistory h;
        h.included = true;
        h.reports = m;
        const std::string s = sectionOf(bundleWith(h), kReportsHeading);
        CHECK(has(s, "newest 10 of 12"));
        CHECK(countOf(s, "hang, ") == 10u);
    }

    // THE UPLOAD STATUS VOCABULARY: each word the uploader writes is carried as it
    // is, a word it does not write is "other", and no sidecar is "none".
    {
        const fs::path words = g_scratch / "case2c" / "crashes";
        const std::vector<std::string> statuses = {"sent",    "duplicate",    "local-only", "backoff",
                                                   "failed",  "abandoned",    "too-large",  "expired",
                                                   "refused", "rate-limited", "gibberish-word"};
        for (std::size_t i = 0; i < statuses.size(); ++i) {
            const fs::path p = words / ("hang-9-" + std::to_string(i) + ".txt");
            writeFile(p, hangReport("hang", 5100, "AAAABBBBCCCCDDDD", 10));
            sidecar(p, statuses[i]);
            setAge(p, 100 + static_cast<int>(i));
        }
        const ReportListing w = listRecentReports(words.string(), now, 20);
        std::vector<std::string> seen;
        for (const ReportSummary& r : w.newest) { seen.push_back(r.upload); }
        std::sort(seen.begin(), seen.end());
        std::vector<std::string> want = statuses;
        want.back() = "other";
        std::sort(want.begin(), want.end());
        CHECK(seen == want);
    }

    // A FILE THAT IS NOT A REPORT is listed as unreadable, not skipped and not
    // guessed at; an empty folder and a missing one say different things.
    {
        const fs::path odd = g_scratch / "case2d" / "crashes";
        writeFile(odd / "crash-bad.txt", "this is not a report\n");
        const ReportListing o = listRecentReports(odd.string(), now);
        CHECK(o.newest.size() == 1u && o.newest[0].kind == "unknown");
        CHECK(has(reportSummaryLine(o.newest[0]), "not a readable report"));

        DiagHistory empty;
        empty.included = true;
        empty.reports = listRecentReports((g_scratch / "case2e" / "crashes").string(), now);
        CHECK(has(sectionOf(bundleWith(empty), kReportsHeading), "none - the reports folder could not be read"));
        fs::create_directories(g_scratch / "case2f" / "crashes");
        empty.reports = listRecentReports((g_scratch / "case2f" / "crashes").string(), now);
        CHECK(has(sectionOf(bundleWith(empty), kReportsHeading),
                  "none - the reports folder holds no crash or freeze report"));
    }

    // A FREEZE REPORT WITH THE LARGEST MODULE LIST THE TABLE HOLDS (256 modules)
    // writes it before the process block, so the uptime sits ~48 KiB in; it is
    // still found, and the read stops at its bound however long the file is.
    {
        const fs::path big = g_scratch / "case2g" / "crashes";
        std::string text = hangReport("hang", 6100, "AAAABBBBCCCCDDDD", 4321);
        const std::size_t at = text.find("--- process ---");
        std::string modules;
        for (int i = 0; i < 256; ++i) {
            modules += "  module" + std::to_string(i) + std::string(60, 'x') +
                       ".dll base=0x00007FF600000000 size=0x100000 pdb=module" + std::to_string(i) +
                       ".pdb build=0123456789ABCDEF0123456789ABCDEF1\n";
        }
        text.insert(at, modules);
        CHECK(at != std::string::npos && text.find("uptime-sec") > 40000u);
        writeFile(big / "hang-5-1.txt", text);
        const ReportListing b = listRecentReports(big.string(), now);
        CHECK(b.newest.size() == 1u && b.newest[0].uptimeSec == 4321);
        // ...and a report whose uptime lies past the read bound reads "unknown".
        std::string longer = hangReport("hang", 6100, "AAAABBBBCCCCDDDD", 4321);
        longer.insert(longer.find("--- process ---"), std::string(kReportReadBytes + 10, 'm') + "\n");
        writeFile(big / "hang-5-2.txt", longer);
        setAge(big / "hang-5-1.txt", 100);
        setAge(big / "hang-5-2.txt", 5);
        const ReportListing c = listRecentReports(big.string(), now);
        CHECK(c.newest.size() == 2u && c.newest[0].uptimeSec == -1);
        CHECK(has(reportSummaryLine(c.newest[0]), "uptime unknown"));
    }

    // AGE WORDING.
    CHECK(agoText(5) == "just now");
    CHECK(agoText(59) == "just now");
    CHECK(agoText(60) == "1 min ago");
    CHECK(agoText(3599) == "59 min ago");
    CHECK(agoText(3600) == "1 h ago");
    CHECK(agoText(86399) == "23 h ago");
    CHECK(agoText(86400) == "1 d ago");
    CHECK(agoText(-1) == "(age unknown)");
}

void testOffAddsNothing() {
    std::printf("diagnostics off: nothing is added\n");
    // The core: no history, no heading - the bundle is what it always was.
    DiagBundleInput in;
    in.logLines.push_back("12:00:00.000 info a line");
    const std::string plain = buildDiagnosticsBundle(in);
    CHECK(!has(plain, "previous session"));
    CHECK(!has(plain, "reports on this machine"));
    CHECK(countOf(plain, "\n--- ") == 1u);  // the log's own heading and no other
}

void testCache() {
    std::printf("the cache reads off the caller's thread, and reads again only when asked\n");
    const fs::path base = g_scratch / "case3";
    const fs::path logDir = base / "logs";
    const fs::path crashDir = base / "crashes";
    seedLogs(logDir);
    seedReports(crashDir);

    DiagHistoryCache cache;
    CHECK(!cache.ready());
    CHECK(cache.readsStarted() == 0u);
    {
        const DiagHistory pending = cache.snapshot();
        CHECK(pending.included && pending.pending);
    }
    cache.refresh(logDir.string(), crashDir.string(), 1000, 5000);
    CHECK(cache.waitIdle(std::chrono::milliseconds(10000)));
    CHECK(cache.ready() && cache.readsStarted() == 1u);
    {
        const DiagHistory h = cache.snapshot();
        CHECK(h.included && !h.pending && h.previous.found && h.reports.total == 3u);
    }
    // Younger than maxAge: no second read.
    cache.refresh(logDir.string(), crashDir.string(), 3000, 5000);
    CHECK(cache.waitIdle(std::chrono::milliseconds(10000)));
    CHECK(cache.readsStarted() == 1u);

    // The previous session never changes, so it is read ONCE: the log files are
    // gone and a new report has arrived, and a later read has the new report and
    // the old session.
    std::error_code ec;
    fs::remove_all(logDir, ec);
    const fs::path fresh = crashDir / "hang-777-1.txt";
    writeFile(fresh, hangReport("hang", 9999, "FFFFEEEEDDDDCCCC", 55));
    setAge(fresh, 1);
    cache.refresh(logDir.string(), crashDir.string(), 7000, 5000);
    CHECK(cache.waitIdle(std::chrono::milliseconds(10000)));
    CHECK(cache.readsStarted() == 2u);
    {
        const DiagHistory h = cache.snapshot();
        CHECK(h.previous.found && has(h.previous.build, "0.99.59"));
        CHECK(h.reports.total == 4u);
        CHECK(!h.reports.newest.empty() && h.reports.newest[0].stalledMs == 9999);
    }
}

// A line's tag in letters: a line cut off at the ring's width is judged as one that
// may carry a frequency, and every digit on it is masked, so a tag made of digits
// would not survive the scrub it is there to be found through.
std::string ringTag(int i) {
    std::string t = "ringtag";
    t += static_cast<char>('a' + (i / 676) % 26);
    t += static_cast<char>('a' + (i / 26) % 26);
    t += static_cast<char>('a' + i % 26);
    return t + "z";
}

// A ring of `n` lines, each the longest the ring holds (191 bytes).
std::vector<std::string> longRing(int n) {
    std::vector<std::string> ring;
    for (int i = 0; i < n; ++i) {
        std::string l = "12:10:" + std::to_string(10 + i % 50) + ".000 info " + ringTag(i) + " ";
        l.resize(DiagLog::kLineBytes - 1, 'x');
        ring.push_back(l);
    }
    return ring;
}

void testTruncation() {
    std::printf("the sections fit beside a full ring, and outlast the log when a report is cut\n");
    const fs::path logDir = g_scratch / "case4" / "logs";
    const fs::path crashDir = g_scratch / "case4" / "crashes";

    // THE WORST CASE THE CAPS ALLOW. The previous session's lines at 190 bytes
    // each (so the byte bound, not the line bound, is what stops it), and ten
    // crash reports each with a reason of the full 120 characters.
    std::string b = line(2, 0, "FoxSDR 0.99.59 (b0b0b0b0b0b0) starting");
    for (int i = 0; i < 200; ++i) {
        std::string l = line(3, i % 60, "mode: session B line " + std::to_string(i) + " ");
        l.pop_back();
        l.resize(190, 'y');
        b += l + "\n";
    }
    b += line(4, 0, "audio: session B's LAST line before the freeze");
    writeFile(logDir / "foxsdr.1.log", b);
    writeFile(logDir / "foxsdr.log", line(10, 0, "FoxSDR 0.99.61 (c0c0c0c0c0c0) starting"));
    for (int i = 0; i < 10; ++i) {
        std::string r = crashReport();
        const std::size_t at = r.find("access violation");
        r.replace(at, std::string("access violation").size(), std::string(150, 'r'));
        const fs::path p = crashDir / ("crash-20260930-10101" + std::to_string(i) + "-1-1.txt");
        writeFile(p, r);
        sidecar(p, "failed");
        setAge(p, 100 + i);
    }
    const DiagHistory h = readDiagHistory(logDir.string(), crashDir.string(), std::time(nullptr));
    CHECK(h.previous.found && h.previous.lines.size() < kPreviousSessionMaxLines);
    CHECK(h.reports.newest.size() == kListedReports);

    const std::string raw = bundleWith(h, longRing(DiagLog::kRingLines));
    const std::string prepared = prepareDiagnosticsForReport(raw);
    std::printf("full ring + the longest sections: raw %zu bytes, prepared %zu bytes (cap %zu)\n",
                raw.size(), prepared.size(), kProblemReportDiagnosticsMaxBytes);
    // It all fits: the ring's 256 lines AND both sections, with nothing dropped.
    CHECK(raw.size() <= kProblemReportDiagnosticsMaxBytes);
    CHECK(prepared == raw);
    CHECK(has(prepared, ringTag(0)));
    CHECK(has(prepared, ringTag(DiagLog::kRingLines - 1)));

    // PAST THE CAP (a ring that grew, a header that did) the OLDEST lines of the
    // current log are what is given up; both sections are kept whole.
    const std::string big = bundleWith(h, longRing(420));
    CHECK(big.size() > kProblemReportDiagnosticsMaxBytes);
    const std::string cut = prepareDiagnosticsForReport(big);
    std::printf("an oversized ring: raw %zu bytes, prepared %zu bytes\n", big.size(), cut.size());
    CHECK(cut.size() <= kProblemReportDiagnosticsMaxBytes);
    CHECK(has(cut, "(earlier lines dropped)"));
    CHECK(!has(cut, ringTag(0)));
    CHECK(has(cut, ringTag(419)));
    CHECK(has(cut, kPreviousSessionHeading));
    CHECK(has(cut, kReportsHeading));
    CHECK(has(cut, "session B's LAST line before the freeze"));
    CHECK(countOf(sectionOf(cut, kReportsHeading), "crash, ") == kListedReports);
}

void testThroughTheWindow() {
    std::printf("through the real AppWindow\n");
    // The window reads the folders the application uses; FOXSDR_DIAG_DIR points
    // that tree at scratch.
    const fs::path base = g_scratch / "case5";
    setEnv("FOXSDR_DIAG_DIR", base.string());
    const fs::path logDir = fs::path(diagLogDir());
    const fs::path crashDir = fs::path(diagCrashDir());
    CHECK(fs::path(diagBaseDir()) == base);
    seedLogs(logDir);
    seedReports(crashDir);

    // --- OFF: nothing is read, nothing is added ---
    {
        cascade::gui::AppWindow off;
        off.setDiagnosticsDir(crashDir.string());
        Access::apply(off, false);
        const std::string b = Access::bundle(off, true);
        CHECK(!has(b, "previous session"));
        CHECK(!has(b, "reports on this machine"));
        CHECK(!has(b, "SECRET"));
        CHECK(Access::reads(off) == 0u);
        // ...and a run that may not touch the disk (no crash dir) is the same
        // with the switch ON.
        cascade::gui::AppWindow tool;
        Access::apply(tool, true);
        const std::string t = Access::bundle(tool, true);
        CHECK(!has(t, "previous session"));
        CHECK(Access::reads(tool) == 0u);
    }

    // --- ON: the way main() starts a session ---
    // main() configures the log, writes the start line, and builds the window;
    // run() then applies the switch. The seeded foxsdr.log already holds session C
    // (this session) from seedLogs; configure() opens it for append.
    {
        cascade::gui::AppWindow on;
        on.setDiagnosticsDir(crashDir.string());
        Access::apply(on, true);  // what run() does first
        const std::string b = Access::bundle(on, true);
        CHECK(Access::reads(on) >= 1u);
        const std::string prev = sectionOf(b, kPreviousSessionHeading);
        CHECK(has(prev, "session: FoxSDR 0.99.59 (b0b0b0b0b0b0)"));
        CHECK(has(prev, "session B's LAST line before the freeze"));
        CHECK(!has(b, "433.917") && !has(b, "00000001") && !has(b, "Alice"));
        const std::string rep = sectionOf(b, kReportsHeading);
        CHECK(has(rep, "newest 3 of 3"));
        CHECK(has(rep, "upload sent"));
        CHECK(has(rep, "upload local-only"));
        CHECK(!has(rep, "SECRET"));

        // THE REAL ORDER OF THE FIELD CASE: a freeze report written NOW, in this
        // session, is in what the person copies next.
        const fs::path now = crashDir / "hang-888-1.txt";
        writeFile(now, hangReport("hang", 123456, "0123456789ABCDEF", 77));
        setAge(now, 0);
        const std::string again = Access::bundle(on, true);
        CHECK(has(sectionOf(again, kReportsHeading), "stalled 123456 ms"));

        // Switching it off part-way: nothing more is added.
        Access::apply(on, false);
        const std::size_t readsBefore = Access::reads(on);
        const std::string off = Access::bundle(on, true);
        CHECK(!has(off, "previous session") && !has(off, "reports on this machine"));
        CHECK(Access::reads(on) == readsBefore);
    }
    // --- A window whose switch went ON part-way through the session cannot tell
    // this session from the one before it, and says so rather than guessing.
    {
        cascade::gui::AppWindow late;
        late.setDiagnosticsDir(crashDir.string());
        Access::apply(late, false);  // the start of the session: off
        Access::apply(late, true);   // ...switched on later
        const std::string b = Access::bundle(late, true);
        const std::string prev = sectionOf(b, kPreviousSessionHeading);
        CHECK(has(prev, "none - diagnostics were switched on part-way through this session"));
        CHECK(!has(prev, "session B"));
        // The reports do not depend on it.
        CHECK(has(sectionOf(b, kReportsHeading), "newest "));
    }
    DiagLog::instance().configure(std::string(), false);
}

}  // namespace

int main() {
    std::printf("test_diag_history\n");
    isolate();
    testPreviousSession();
    testReports();
    testOffAddsNothing();
    testCache();
    testTruncation();
    testThroughTheWindow();
    return testSummary("test_diag_history");
}
