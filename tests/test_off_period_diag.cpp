// NOTHING FROM A SWITCHED-OFF PERIOD IS EVER SENT - the Diagnostics family and the shared building blocks (0.99.115, brief 79; the rule R of the owner's thread).
//
// R: nothing that happened while a data switch was OFF is ever sent, even after that switch is turned ON again. While OFF nothing of its family is collected, counted or written for
// later sending; turning it ON starts from empty. Each case below is one COLLECTOR of the Diagnostics family (or a block both families share), tested in the form
// "ON, activity, OFF, activity, ON, activity: what a later report can hold is ONLY the last ON period's":
//
//   D1  issue reports met while capture was off (the `pending` words)        -> forgotten, never filed when capture is armed again
//   D1b a word met BEFORE the start-up decision is still filed if the decision is ON (the one legitimate buffer)
//   D2  crash / freeze / issue reports on disk, unsent, when the switch goes off -> withheld (local-only sidecar): the file stays, the sweep never takes it
//   D3  the log ring a report carries        -> only the lines written since that family's floor (crash path reader included); the person's own copy is whole
//   D4  the event trail a report carries     -> emptied at the edges
//   D5  the stall ledger and the health ledger -> forgotten at an edge, staying armed
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
#include "core/event_trail.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "core/telemetry.hpp"
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

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::vector<fs::path> issueFiles(const fs::path& dir) {
    (void)issue::waitForWritesForTest(3000);
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("issue-", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) { out.push_back(e.path()); }
    }
    return out;
}

bool anyLineHas(const std::vector<std::string>& lines, const char* what) {
    for (const std::string& l : lines) { if (l.find(what) != std::string::npos) { return true; } }
    return false;
}

std::string statusOf(const fs::path& report) {
    const std::string text = readFile(uploadSidecarPath(report.string()));
    const std::size_t at = text.find("status: ");
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find('\n', at);
    return text.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
}

}  // namespace

int main() {
    std::printf("test_off_period_diag\n");
    const fs::path dir = fs::temp_directory_path() / ("cascade-offperiod-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // --- D1: a word met while capture is OFF (after the start-up decision) is forgotten, not kept for the next ON -----------------------------------
    {
        std::printf("  D1  issue words met while Diagnostics is off are never filed when it is switched on\n");
        issue::resetIssueReportsForTest();
        CrashHandlerConfig off;
        off.crashDir = dir.string();
        off.enabled = false;
        off.exitAfterReport = false;
        installCrashHandlers(off);
        CHECK(activeCrashDir().empty());
        issue::filePendingIssueReports();                        // the start-up decision: OFF
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());   // met while OFF
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.exitAfterReport = false;
        installCrashHandlers(on);                                // switched ON again
        issue::filePendingIssueReports();                        // what the windows ask at that edge
        CHECK(issueFiles(dir).empty());                          // the word from the OFF period is not filed
        CHECK(issue::takeNewlyFiled() == 0);
        // ...and a word met NOW (an ON period) is filed as always
        CHECK(!issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).size() == 1);
        for (const fs::path& p : issueFiles(dir)) { fs::remove(p, ec); }
        (void)issue::takeNewlyFiled();
        // discardPendingIssueReports at an edge: a word noted by an undecided process and discarded is not filed either
        issue::resetIssueReportsForTest();
        installCrashHandlers(off);
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());   // undecided: kept (the start-up buffer)
        issue::discardPendingIssueReports();
        installCrashHandlers(on);
        issue::filePendingIssueReports();
        CHECK(issueFiles(dir).empty());
    }
    issue::resetIssueReportsForTest();

    // --- D1b: the start-up buffer: a word met BEFORE the decision is filed when the decision is ON --------------------------------------------------
    {
        std::printf("  D1b a word met before the start-up decision is filed when the decision is ON (the one legitimate buffer)\n");
        issue::resetIssueReportsForTest();
        CrashHandlerConfig off;
        off.crashDir = dir.string();
        off.enabled = false;
        off.exitAfterReport = false;
        installCrashHandlers(off);
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());   // no decision made yet: waits
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        issue::filePendingIssueReports();                              // the decision: ON
        CHECK(issueFiles(dir).size() == 1);
        for (const fs::path& p : issueFiles(dir)) { fs::remove(p, ec); }
        (void)issue::takeNewlyFiled();
        // the same start, decision OFF: the word is dropped, not kept for later
        issue::resetIssueReportsForTest();
        installCrashHandlers(off);
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());
        issue::filePendingIssueReports();                              // the decision: OFF -> dropped
        installCrashHandlers(on);
        issue::filePendingIssueReports();
        CHECK(issueFiles(dir).empty());
    }
    issue::resetIssueReportsForTest();

    // --- D2: unsent reports on disk are withheld when the switch goes off ----------------------------------------------------------------------------
    {
        std::printf("  D2  unsent crash, freeze and issue reports are withheld (local-only) at the OFF edge; the files stay\n");
        const fs::path rd = dir / "reports";
        fs::create_directories(rd, ec);
        const fs::path crash = rd / "crash-20261124-101010-1-1.txt";
        const fs::path hang = rd / "hang-1234-20261124-101011.txt";
        const fs::path iss = rd / "issue-20261124-101012-1-1.txt";
        const fs::path sentAlready = rd / "crash-20261101-000000-1-1.txt";
        const fs::path bundle = rd / "diagnostics.txt";
        const fs::path dump = rd / "crash-20261124-101010-1-1.dmp";
        for (const fs::path& p : {crash, hang, iss, sentAlready, bundle, dump}) { writeFile(p, "FoxSDR report body\nkind: crash\n"); }
        writeFile(uploadSidecarPath(sentAlready.string()), "status: sent\nattempts: 1\nat: 2026-11-01T00:00:00Z\nsignature: (none)\nnote: \n");
        CHECK(statusOf(crash).empty() && statusOf(hang).empty() && statusOf(iss).empty());
        const int n = withholdUnsentReports(rd.string(), 1795000000ull);
        CHECK(n == 3);
        CHECK(statusOf(crash) == "local-only");
        CHECK(statusOf(hang) == "local-only");
        CHECK(statusOf(iss) == "local-only");
        CHECK(statusOf(sentAlready) == "sent");               // a settled one is left as it was
        CHECK(statusOf(bundle).empty());                      // the person's own bundle and the memory dump are not ours to mark
        CHECK(!fs::exists(uploadSidecarPath(dump.string())));
        CHECK(fs::exists(crash) && fs::exists(hang) && fs::exists(iss));   // the person's own copy stays
        CHECK(withholdUnsentReports(rd.string(), 1795000001ull) == 0);     // idempotent
        // the sweep, switched ON again, looks at none of them: not one is considered, so no connection is made for them
        SweepParams p;
        p.crashDir = rd.string();
        p.url = "http://127.0.0.1:9/";
        p.installId = "";
        p.enabled = true;
        p.nowEpoch = 1795000002ull;
        const SweepOutcome o = sweepCrashDir(p, std::make_shared<UploadCancel>());
        CHECK(o.considered == 0);
        CHECK(o.sent == 0 && o.failed == 0);
        // a report written AFTER the edge (an ON period) is a candidate as always
        const fs::path fresh = rd / "crash-20261124-111111-1-1.txt";
        writeFile(fresh, "FoxSDR report body\nkind: crash\n");
        const SweepOutcome o2 = sweepCrashDir(p, std::make_shared<UploadCancel>());
        CHECK(o2.considered == 1);
        // a folder that does not exist is no error
        CHECK(withholdUnsentReports((dir / "nowhere").string(), 1795000003ull) == 0);
        CHECK(withholdUnsentReports(std::string(), 1795000003ull) == 0);
    }

    // --- D3: the log ring ---------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  D3  a report reads the ring only from its family's floor; the person's own copy is whole\n");
        DiagLog& log = DiagLog::instance();
        log.resetForTest();
        diagLogf("rtlsdr: line-before-everything");
        log.markReportFloor(DiagLog::ReportFamily::Diagnostics);        // Diagnostics switched ON (or OFF) here
        diagLogf("rtlsdr: line-after-diag-floor");
        log.markReportFloor(DiagLog::ReportFamily::Usage);              // Usage switched ON here
        diagLogf("rtlsdr: line-after-usage-floor");
        const std::vector<std::string> whole = log.ringSnapshot();
        const std::vector<std::string> d = log.ringSnapshotFor(DiagLog::ReportFamily::Diagnostics);
        const std::vector<std::string> u = log.ringSnapshotFor(DiagLog::ReportFamily::Usage);
        CHECK(whole.size() == 3);                                       // the person's own copy is not touched
        CHECK(!anyLineHas(d, "line-before-everything"));
        CHECK(anyLineHas(d, "line-after-diag-floor") && anyLineHas(d, "line-after-usage-floor"));
        CHECK(!anyLineHas(u, "line-before-everything") && !anyLineHas(u, "line-after-diag-floor"));
        CHECK(anyLineHas(u, "line-after-usage-floor"));
        // the crash path's own reader (no lock, no allocation) follows the Diagnostics floor
        char raw[4096] = {};
        const std::size_t used = log.copyRingRaw(raw, sizeof(raw));
        CHECK(used > 0);
        const std::string rawText(raw);
        CHECK(rawText.find("line-before-everything") == std::string::npos);
        CHECK(rawText.find("line-after-diag-floor") != std::string::npos);
        // a ring that has wrapped past the floor: the floor holds (the floor is a line COUNT, not a slot)
        for (int i = 0; i < 400; ++i) { diagLogf("rtlsdr: filler %d", i); }
        const std::vector<std::string> wrapped = log.ringSnapshotFor(DiagLog::ReportFamily::Usage);
        CHECK(wrapped.size() <= 256);
        CHECK(!anyLineHas(wrapped, "line-before-everything"));
        log.resetForTest();
        // after a reset both floors are back to "everything" (a test hook only)
        diagLogf("rtlsdr: fresh");
        CHECK(anyLineHas(log.ringSnapshotFor(DiagLog::ReportFamily::Diagnostics), "fresh"));
        CHECK(anyLineHas(log.ringSnapshotFor(DiagLog::ReportFamily::Usage), "fresh"));
        log.resetForTest();
    }

    // --- D4: the event trail ------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  D4  the event trail a report carries is emptied at the edges\n");
        trail::resetForTest();
        trail::noteSource(true, "rtlsdr");
        trail::noteMode(2);
        CHECK(!trail::formatNow().empty());
        trail::clear();
        CHECK(trail::formatNow().empty());
        trail::noteMode(3);                                             // an ON period after the edge
        CHECK(!trail::formatNow().empty());
        trail::resetForTest();
        CHECK(trail::formatNow().empty());                              // the test seam empties it as the edge does (review 4 D7: one body for both)
        trail::noteMode(3);
        trail::resetForTest();
        CHECK(trail::formatNow().empty());
    }

    // --- D5: the two ledgers ------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  D5  the stall ledger and the health ledger are forgotten by an edge and stay armed\n");
        const std::string id = newInstallId();
        const fs::path stallFile = dir / "telemetry-stalls";
        StallLedger sl;
        sl.arm(stallFile.string(), id, false);
        sl.note();
        sl.note();
        CHECK(sl.count() == 2);
        sl.forget();
        CHECK(sl.armed());
        CHECK(sl.count() == 0);
        CHECK(!fs::exists(stallFile));                                  // a count of nothing is no file (the ledger's own rule)
        sl.note();
        CHECK(sl.count() == 1);                                         // counts again after the edge
        StallLedger unarmed;
        unarmed.forget();                                               // harmless on a disarmed ledger
        CHECK(!unarmed.armed() && unarmed.count() == 0);

        const fs::path healthFile = dir / "telemetry-health";
        health::HealthLedger hl;
        hl.arm(healthFile.string(), id, false);
        hl.note(health::tokenScanNone());
        hl.note(health::tokenScanNone());
        CHECK(!hl.counts().empty());
        CHECK(hl.flush());
        hl.forget();
        CHECK(hl.armed());
        CHECK(hl.counts().empty() && hl.priorCounts().empty());
        CHECK(hl.encoded().empty() || hl.encoded() == health::encode(health::Counts()));
        CHECK(hl.flush());
        health::Counts fromDisk;
        health::HealthLedger::parseFileText(readFile(healthFile), id, fromDisk);
        CHECK(fromDisk.empty());
        hl.note(health::tokenScanNone());
        CHECK(!hl.counts().empty());                                    // counts again after the edge
    }

    // --- D6 (review 4): a marker that could not be written is not a mark ---------------------------------------------------------------------------------
    {
        std::printf("  D6  a report whose local-only marker could not be written is not counted as withheld (and the failure is in the log)\n");
        const fs::path rd = dir / "reports-d6";
        fs::create_directories(rd, ec);
        const fs::path good = rd / "crash-20261124-121212-1-1.txt";
        const fs::path bad = rd / "crash-20261124-121213-1-1.txt";
        writeFile(good, "FoxSDR report body\nkind: crash\n");
        writeFile(bad, "FoxSDR report body\nkind: crash\n");
        // the marker's own path is a FOLDER: the write fails, and the report stays sendable if Diagnostics is switched on again
        fs::create_directories(uploadSidecarPath(bad.string()), ec);
        DiagLog::instance().resetForTest();
        const int n = withholdUnsentReports(rd.string(), 1795000010ull);
        CHECK(n == 1);                                                  // only the one that was marked
        CHECK(statusOf(good) == "local-only");
        bool logged = false;
        for (const std::string& line : DiagLog::instance().ringSnapshot()) {
            if (line.find("could not be marked") != std::string::npos) { logged = true; }
        }
        CHECK(logged);
        fs::remove_all(uploadSidecarPath(bad.string()), ec);
    }

    fs::remove_all(dir, ec);
    return testSummary("test_off_period_diag");
}
