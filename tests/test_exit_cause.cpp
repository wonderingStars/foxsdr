// HOW THE PREVIOUS SESSION ENDED (0.99.69, core/exit_cause.hpp): the rule that
// sorts an unclean exit into died / killed / ended / unknown, the four lifetime
// counters that must always add up, and the persistence of both.
//
// WHAT THIS FILE PINS:
//   - the counters' invariant, at every step of every sequence: an unclean exit is
//     `unknown` the moment it is counted and refine() only ever MOVES it, so the total
//     is the number of unclean exits and no class is counted without one behind it;
//   - every row of the rule, from hand-built listings (so each row is exercised
//     alone), including the window edges, the faults a session survived, the
//     precedence of an ending over a freeze, and the cases with nothing to read;
//   - the rule against REAL reports: every class of sentinel report is written by the
//     real finishSentinelWatch into a folder, read back by the real listRecentReports
//     and classified - so the words the rule matches on are the words the writer writes;
//   - that the config carries the four counters and the session start, and refuses a
//     hand-edited negative number like every other counter.
//
// The processes - the real application killed and restarted, and the counters read
// back from its config - are in test_exit_cause_app.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/breadcrumb.hpp"
#include "core/config.hpp"
#include "core/crash_handler.hpp"
#include "core/diag_history.hpp"
#include "core/exit_cause.hpp"
#include "core/sentinel.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;
using breadcrumb::Phase;

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
    const fs::path dir = base / (std::string("cascade-exitcause-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Hand-built listings. Times are plain numbers: the previous session began at
// 1000, this one at 2000, so a report at 1500 is the previous session's.
// ---------------------------------------------------------------------------
constexpr std::int64_t kPrev = 1000;
constexpr std::int64_t kThis = 2000;

ReportSummary crashReport(const std::string& reason, const std::string& code, std::int64_t at) {
    ReportSummary r;
    r.kind = "crash";
    r.reason = reason;
    r.code = code;
    r.writtenEpoch = at;
    return r;
}

ReportSummary freezeReport(const char* kind, std::int64_t at) {
    ReportSummary r;
    r.kind = kind;
    r.stalledMs = 9000;
    r.writtenEpoch = at;
    return r;
}

// Newest first, as listRecentReports returns them.
ReportListing listing(std::vector<ReportSummary> newestFirst, bool readable = true) {
    ReportListing l;
    l.readable = readable;
    l.total = newestFirst.size();
    l.newest = std::move(newestFirst);
    return l;
}

ExitVerdict classify(const ReportListing& l) { return classifyPreviousExit(l, kPrev, kThis); }

const std::string kSentCrash = kSentinelReasonCrash;
const std::string kSentFrozen = kSentinelReasonFrozen;
const std::string kSentStartup = kSentinelReasonStartup;
const std::string kSentOutside = kSentinelReasonOutside;
const std::string kSentSession = kSentinelReasonSession;

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------
void testCountersAlwaysAddUp() {
    ExitCounts c;
    CHECK(c.total() == 0);
    CHECK(!c.refine(ExitClass::Killed));  // nothing unclean has been counted: nothing to move

    // A long, mixed sequence: every step is an unclean exit, refined to a class chosen
    // by a fixed pattern, sometimes not refined at all (the session was ended before
    // the evidence was read), sometimes "refined" to unknown. The total is the number
    // of unclean exits after EVERY step, and a class never exceeds what was counted.
    const ExitClass pattern[] = {ExitClass::Died,   ExitClass::Killed, ExitClass::Unknown,
                                 ExitClass::Ended,  ExitClass::Killed, ExitClass::Died,
                                 ExitClass::Unknown, ExitClass::Died};
    std::uint64_t uncleanExits = 0;
    for (int i = 0; i < 200; ++i) {
        c.noteUnclean();
        ++uncleanExits;
        CHECK(c.total() == uncleanExits);
        CHECK(c.unknown >= 1);  // counted as unknown at once, before any evidence
        if (i % 5 == 3) { continue; }  // ended again before the evidence was read
        const ExitClass cls = pattern[i % 8];
        const bool moved = c.refine(cls);
        CHECK(moved == (cls != ExitClass::Unknown));
        CHECK(c.total() == uncleanExits);
    }
    CHECK(c.total() == uncleanExits);
    CHECK(c.of(ExitClass::Died) + c.of(ExitClass::Killed) + c.of(ExitClass::Ended) +
              c.of(ExitClass::Unknown) == uncleanExits);
    CHECK(c.died > 0 && c.killed > 0 && c.ended > 0 && c.unknown > 0);

    // refine() with no unknown left moves nothing, and so cannot invent an exit.
    ExitCounts d;
    d.noteUnclean();
    CHECK(d.refine(ExitClass::Died));
    CHECK(!d.refine(ExitClass::Died));  // a second refine of the same exit
    CHECK(d.died == 1 && d.total() == 1);

    // The words of the log line and of the counters' names.
    CHECK(std::string(exitClassId(ExitClass::Died)) == "died");
    CHECK(std::string(exitClassId(ExitClass::Killed)) == "killed");
    CHECK(std::string(exitClassId(ExitClass::Ended)) == "ended");
    CHECK(std::string(exitClassId(ExitClass::Unknown)) == "unknown");
}

// ---------------------------------------------------------------------------
// The rule, one row at a time
// ---------------------------------------------------------------------------
void testRuleRows() {
    // The process's own handler wrote it: died.
    {
        const ExitVerdict v = classify(listing({crashReport("access violation", "0xC0000005", 1500)}));
        CHECK(v.cls == ExitClass::Died);
        CHECK(contains(v.detail, "crash report written by the process"));
    }
    // The sentinel's five classes.
    {
        const ExitVerdict v = classify(listing({crashReport(kSentCrash + " - fast-fail", "0xC0000409", 1500)}));
        CHECK(v.cls == ExitClass::Died);
        CHECK(contains(v.detail, "fast-fail"));
    }
    CHECK(classify(listing({crashReport(kSentFrozen + " - ended by another process", "0x00000001", 1500)})).cls ==
          ExitClass::Died);
    {
        const ExitVerdict v = classify(listing({crashReport(kSentOutside + " - ended by another process", "0x00000001", 1500)}));
        CHECK(v.cls == ExitClass::Killed);
        CHECK(contains(v.detail, "taskkill"));
    }
    // A closed console window: STATUS_CONTROL_C_EXIT is a request to end, not a fault.
    {
        const ExitVerdict v = classify(listing({crashReport(kSentOutside, "0xC000013A", 1500)}));
        CHECK(v.cls == ExitClass::Killed);
        CHECK(contains(v.detail, "console close"));
    }
    CHECK(classify(listing({crashReport(kSentSession, "0x00000001", 1500)})).cls == ExitClass::Ended);
    // Before the first frame, by what: the exit code decides.
    CHECK(classify(listing({crashReport(kSentStartup, "0xC0000005", 1500)})).cls == ExitClass::Died);
    CHECK(classify(listing({crashReport(kSentStartup, "0xE06D7363", 1500)})).cls == ExitClass::Died);
    CHECK(classify(listing({crashReport(kSentStartup, "0x00000001", 1500)})).cls == ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentStartup, "0xFFFFFFFF", 1500)})).cls == ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentStartup, "0xC000013A", 1500)})).cls == ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentStartup, "", 1500)})).cls == ExitClass::Unknown);  // Linux: no code
    // A sentinel sentence a later build writes, and this one has never heard of.
    CHECK(classify(listing({crashReport("sentinel: ended in some new way", "0x00000001", 1500)})).cls ==
          ExitClass::Unknown);

    // A freeze report with nothing after it.
    CHECK(classify(listing({freezeReport("hang", 1500)})).cls == ExitClass::Died);
    CHECK(classify(listing({freezeReport("stall", 1500)})).cls == ExitClass::Killed);
    // ...and nothing at all, or nothing readable.
    CHECK(classify(listing({})).cls == ExitClass::Unknown);
    {
        ReportSummary junk;
        junk.kind = "unknown";
        junk.writtenEpoch = 1500;
        CHECK(classify(listing({junk})).cls == ExitClass::Unknown);
    }
}

void testRulePrecedenceAndWindow() {
    // A session's ENDING beats a freeze, whichever is newer: a freeze that
    // recovered and was then ended from outside has the ending's own report, and
    // that is what ended it.
    CHECK(classify(listing({freezeReport("hang", 1600), crashReport(kSentOutside, "0x00000001", 1500)})).cls ==
          ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentOutside, "0x00000001", 1600), freezeReport("hang", 1500)})).cls ==
          ExitClass::Killed);
    // Two endings in the window (two copies sharing the folder): the newest.
    CHECK(classify(listing({crashReport(kSentSession, "0x1", 1700),
                            crashReport("access violation", "0xC0000005", 1600)})).cls == ExitClass::Ended);

    // FAULTS THE SESSION SURVIVED say nothing of how it ended: an absorbed vendor
    // fault, the parent's report of an enumeration child's death, the child's own
    // report of it. With only those in the window there is no evidence.
    const std::vector<ReportSummary> survived = {
        crashReport(std::string(kAbsorbedFaultReasonPrefix) + " in x.dll", "0xC0000005", 1500),
        crashReport(std::string(kChildDeathReasonPrefix) + " (contained: the parent survived and re-probed)",
                    "0xC0000005", 1500),
        crashReport("child process fault (contained)", "0xC0000005", 1500),
        crashReport("access violation - enumeration child, driver=uhd, attempt 1 (contained)", "0xC0000005", 1500),
    };
    for (const ReportSummary& r : survived) {
        CHECK(reportReasonIsSurvivedFault(r.reason));
        CHECK(classify(listing({r})).cls == ExitClass::Unknown);
    }
    // ...and they do not hide a death that came after them.
    {
        std::vector<ReportSummary> l = survived;
        l.insert(l.begin(), crashReport(kSentOutside, "0x00000001", 1600));
        CHECK(classify(listing(l)).cls == ExitClass::Killed);
    }
    CHECK(!reportReasonIsSurvivedFault("access violation"));
    CHECK(!reportReasonIsSurvivedFault(kSentOutside));
    CHECK(!reportReasonIsSurvivedFault(" - enumeration child, (contained)"));  // the clause IS the reason: names no fault

    // THE WINDOW. A report written before the previous session began is an older
    // ending's; one written after this session began (plus the slack) is not the
    // previous session's. Both edges are inclusive of the session starts.
    CHECK(classify(listing({crashReport(kSentOutside, "0x1", kPrev - 1)})).cls == ExitClass::Unknown);
    CHECK(classify(listing({crashReport(kSentOutside, "0x1", kPrev)})).cls == ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentOutside, "0x1", kThis)})).cls == ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentOutside, "0x1", kThis + 5)})).cls == ExitClass::Killed);
    CHECK(classify(listing({crashReport(kSentOutside, "0x1", kThis + 6)})).cls == ExitClass::Unknown);
    // A report with no time (the file's clock could not be read) is never matched.
    CHECK(classify(listing({crashReport(kSentOutside, "0x1", 0)})).cls == ExitClass::Unknown);
    // An older ending beside the right one does not change the answer.
    CHECK(classify(listing({crashReport(kSentSession, "0x1", 1500),
                            crashReport("access violation", "0xC0000005", 900)})).cls == ExitClass::Ended);

    // NOTHING TO READ: no recorded start (an older build's run), an unreadable folder.
    {
        const ExitVerdict v = classifyPreviousExit(listing({crashReport(kSentOutside, "0x1", 1500)}), 0, kThis);
        CHECK(v.cls == ExitClass::Unknown);
        CHECK(contains(v.detail, "older version"));
    }
    {
        const ExitVerdict v = classify(listing({crashReport(kSentOutside, "0x1", 1500)}, /*readable=*/false));
        CHECK(v.cls == ExitClass::Unknown);
        CHECK(contains(v.detail, "could not be read"));
    }
    CHECK(unknownExit("because").cls == ExitClass::Unknown);

    // THE LOG LINE: one line, the class first, the reason in brackets.
    {
        const ExitVerdict v = classify(listing({crashReport(kSentOutside, "0xC000013A", 1500)}));
        const std::string line = previousExitLogLine(v);
        CHECK(line.rfind("previous session ended: killed (", 0) == 0);
        CHECK(line.find('\n') == std::string::npos);
        CHECK(line.back() == ')');
    }
    CHECK(previousExitLogLine(unknownExit("Diagnostics were off")) ==
          "previous session ended: unknown (Diagnostics were off)");
}

// ---------------------------------------------------------------------------
// The rule against REAL reports
// ---------------------------------------------------------------------------
breadcrumb::Snapshot freshCrumb(Phase p, std::uint64_t silentMs, std::uint32_t flags = 0,
                                std::uint64_t frames = 600) {
    breadcrumb::Snapshot s;
    s.valid = true;
    const std::uint64_t now = breadcrumb::nowMs();
    s.startedMs = now - 60000;
    s.phase = p;
    s.frames = frames;
    s.beatMs = frames > 0 ? now - silentMs : 0;
    s.phaseMs = now - 50000;
    s.flags = flags;
    return s;
}

SentinelEnd endOf(const fs::path& crashDir, const fs::path& logDir, unsigned long pid,
                  unsigned long code, const breadcrumb::Snapshot& c) {
    SentinelEnd e;
    e.appPid = pid;
    e.exitKnown = true;
    e.exitCode = code;
    e.crumb = c;
    e.crashDir = crashDir.string();
    e.logDir = logDir.string();
    e.uptimeSec = 77;
    return e;
}

void testRealSentinelReports() {
    const fs::path logs = scratchDir("logs");
    writeFile(logs / "foxsdr.log",
              "12:00:00.000 info FoxSDR 0.99.69 (abc123def456) starting\n12:00:01.000 info the last line\n");

    // The breadcrumb is built when the row RUNS (it is stamped with the clock; one built when
    // this table was would be stale by the time it is used).
    struct Crumb {
        Phase phase;
        std::uint64_t silentMs;
        std::uint32_t flags;
        std::uint64_t frames;
    };
    struct Row {
        const char* name;
        unsigned long code;
        Crumb crumb;
        SentinelClass sentinel;
        ExitClass expect;
    };
    const Row rows[] = {
        {"crash exit code the handler never saw", 0xC0000409ul, {Phase::Running, 10, 0, 600},
         SentinelClass::Crash, ExitClass::Died},
        {"exit code 0 before the shutdown finished", 0ul, {Phase::Running, 10, 0, 600},
         SentinelClass::Crash, ExitClass::Died},
        {"window stopped drawing, then ended", 1ul, {Phase::Running, 8000, 0, 600},
         SentinelClass::Frozen, ExitClass::Died},
        {"ended from outside, drawing (taskkill)", 1ul, {Phase::Running, 10, 0, 600},
         SentinelClass::Outside, ExitClass::Killed},
        {"ended from outside, drawing (Stop-Process)", 0xFFFFFFFFul, {Phase::Running, 10, 0, 600},
         SentinelClass::Outside, ExitClass::Killed},
        {"console window closed, shutdown unfinished", 0xC000013Aul, {Phase::Running, 10, 0, 600},
         SentinelClass::Outside, ExitClass::Killed},
        {"user-held window ended from outside", 1ul, {Phase::Running, 9000, 0, 600},
         SentinelClass::Outside, ExitClass::Killed},
        {"ended before the first frame, access violation", 0xC0000005ul, {Phase::BuildingApp, 10, 0, 0},
         SentinelClass::Startup, ExitClass::Died},
        {"ended before the first frame, taskkill", 1ul, {Phase::BuildingApp, 10, 0, 0},
         SentinelClass::Startup, ExitClass::Killed},
        {"the session was closing", 1ul, {Phase::Running, 10, breadcrumb::kFlagSessionEnding, 600},
         SentinelClass::Session, ExitClass::Ended},
    };

    unsigned long pid = 7100;
    for (const Row& row : rows) {
        const fs::path dir = scratchDir("real");
        // The user-held row needs the breadcrumb's "user paced" activity to be
        // excused from "frozen": set it for that row only.
        breadcrumb::Snapshot c = freshCrumb(row.crumb.phase, row.crumb.silentMs, row.crumb.flags, row.crumb.frames);
        if (std::string(row.name).rfind("user-held", 0) == 0) { c.activity |= breadcrumb::kUserPaced; }
        const SentinelOutcome o = finishSentinelWatch(endOf(dir, logs, ++pid, row.code, c));
        CHECK(o.verdict.cls == row.sentinel);
        CHECK(!o.reportPath.empty());
        if (o.reportPath.empty()) { continue; }

        // The REAL listing of the folder the report is in, and the rule over it.
        const std::time_t now = std::time(nullptr);
        const ReportListing l = listRecentReports(dir.string(), now);
        CHECK(l.readable && l.newest.size() == 1);
        if (l.newest.size() != 1) { continue; }
        CHECK(l.newest[0].writtenEpoch > 0 && l.newest[0].writtenEpoch <= static_cast<std::int64_t>(now));
        const std::int64_t at = l.newest[0].writtenEpoch;
        const ExitVerdict v = classifyPreviousExit(l, at - 60, at);
        if (v.cls != row.expect) {
            std::printf("FAIL %s: report reason \"%s\" code \"%s\" classified %s, wanted %s (%s)\n",
                        row.name, l.newest[0].reason.c_str(), l.newest[0].code.c_str(),
                        exitClassId(v.cls), exitClassId(row.expect), v.detail.c_str());
        }
        CHECK(v.cls == row.expect);
        // The report belongs to a window that began before it and not after it.
        CHECK(classifyPreviousExit(l, at + 1, at + 100).cls == ExitClass::Unknown);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // THE IN-PROCESS HANDLER'S OWN REPORT, in the shape its writer gives it, beside
    // a sentinel that wrote nothing because of it (one report per death).
    {
        const fs::path dir = scratchDir("inproc");
        writeFile(dir / "crash-20260105-120000-6002-1.txt",
                  "kind: crash\nreason: access violation\ncode: 0xC0000005\n"
                  "address: cascade.exe+0x1\nsignature: A31F00112233445A\nthread: 1\n"
                  "--- context ---\nversion: 0.99.69\n--- stack (thread 1) ---\n");
        const std::time_t now = std::time(nullptr);
        const ReportListing l = listRecentReports(dir.string(), now);
        CHECK(l.newest.size() == 1);
        const ExitVerdict v = classifyPreviousExit(l, static_cast<std::int64_t>(now) - 60,
                                                   static_cast<std::int64_t>(now));
        CHECK(v.cls == ExitClass::Died);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    // A freeze the watchdog filed, as the listing reads it.
    {
        const fs::path dir = scratchDir("hang");
        writeFile(dir / "hang-7001-1.txt", "kind: hang\nstalled-ms: 9000\nsignature: A31F00112233445A\n");
        writeFile(dir / "hang-7002-1.txt", "kind: stall\nstalled-ms: 9000\nsignature: A31F00112233445B\n");
        const std::time_t now = std::time(nullptr);
        const ReportListing l = listRecentReports(dir.string(), now);
        CHECK(l.newest.size() == 2);
        // Two freeze reports of different kinds: the newest by the listing's order.
        // Both are freezes, neither is an ending; the first in the listing decides.
        const ExitVerdict v = classifyPreviousExit(l, static_cast<std::int64_t>(now) - 60,
                                                   static_cast<std::int64_t>(now));
        CHECK(v.cls == (l.newest[0].kind == "hang" ? ExitClass::Died : ExitClass::Killed));
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(logs, ec); }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void testConfigCarriesTheCounters() {
    const fs::path dir = scratchDir("config");
    const std::string path = (dir / "config.json").string();

    AppConfig cfg;
    cfg.telemetryCrashes = 9;
    cfg.telemetryExitsDied = 2;
    cfg.telemetryExitsKilled = 3;
    cfg.telemetryExitsEnded = 1;
    cfg.telemetryExitsUnknown = 3;
    cfg.telemetrySessionStarted = 1790000000;
    std::string err;
    CHECK(ConfigStore::save(path, cfg, err));
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.telemetryCrashes == 9);
    CHECK(back.telemetryExitsDied == 2);
    CHECK(back.telemetryExitsKilled == 3);
    CHECK(back.telemetryExitsEnded == 1);
    CHECK(back.telemetryExitsUnknown == 3);
    CHECK(back.telemetrySessionStarted == 1790000000);

    // A file an earlier build wrote has none of them: zero, and no complaint.
    writeFile(dir / "old.json", "{\n  \"schemaVersion\": 1,\n  \"telemetryCrashes\": 4\n}\n");
    AppConfig old;
    CHECK(ConfigStore::load((dir / "old.json").string(), old, err));
    CHECK(old.telemetryCrashes == 4);
    CHECK(old.telemetryExitsDied == 0 && old.telemetryExitsKilled == 0 && old.telemetryExitsEnded == 0 &&
          old.telemetryExitsUnknown == 0 && old.telemetrySessionStarted == 0);

    // A hand-edited negative or fractional number is not a count.
    writeFile(dir / "edited.json",
              "{\n  \"schemaVersion\": 1,\n  \"telemetryExitsDied\": -3,\n"
              "  \"telemetryExitsKilled\": 1.5,\n  \"telemetryExitsEnded\": \"7\",\n"
              "  \"telemetrySessionStarted\": -1\n}\n");
    AppConfig edited;
    CHECK(ConfigStore::load((dir / "edited.json").string(), edited, err));
    CHECK(edited.telemetryExitsDied == 0 && edited.telemetryExitsKilled == 0 && edited.telemetryExitsEnded == 0 &&
          edited.telemetrySessionStarted == 0);
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

}  // namespace

int main() {
    testCountersAlwaysAddUp();
    testRuleRows();
    testRulePrecedenceAndWindow();
    testRealSentinelReports();
    testConfigCarriesTheCounters();
    return testSummary("test_exit_cause");
}
