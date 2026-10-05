// diag_history.hpp - what a bug report can say about the sessions BEFORE this one:
// the end of the previous session's log, and the crash and freeze reports this
// machine holds.
//
// THE FIELD CASE (0.99.59). A window froze after twenty minutes. The user ended
// it from the taskbar, restarted, and sent the bundle five minutes into the next
// session. The bundle described a healthy five-minute session; the one that
// froze was gone from it (the log ring is in memory and holds the CURRENT
// session only), and nothing in it said whether a freeze report had been written
// or uploaded. Two additions answer that:
//
//   - the last lines of the PREVIOUS session's log, read from the rotating log
//     files (foxsdr.log, foxsdr.1.log, foxsdr.2.log), under a heading of its
//     own, saying plainly when there is none;
//   - one line per report on the machine (the newest ten crash-* / hang-*
//     files): its kind, its age, the version that wrote it, its uptime, how long
//     the interface was stalled or what the fault was, its signature, and what
//     became of its upload. No stack, no log, no path.
//
// WHAT IS NEVER DONE HERE. This file reads and shapes; it SCRUBS nothing - every
// line it hands over is passed through core::scrubUploadLog / scrubUploadLine
// by buildDiagnosticsBundle, the one point the current log passes through - and
// it opens nothing unless the caller asked it to, which the caller does only
// while diagnostics are on. The bundle is only ever sent by the user's own
// action; none of this is in the automatic crash upload.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DIAG_HISTORY_HPP
#define CASCADE_CORE_DIAG_HISTORY_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::core {

// How much of the previous session's log is carried. Bounded by lines AND bytes
// (whichever bites first; the NEWEST lines are kept), sized against the caps the
// bundle already lives under: the current log is at most 256 lines of 191 bytes,
// the problem-report attachment is capped at 65536 bytes after scrubbing, and its
// truncation drops the OLDEST lines of the current log first, so even this at its
// ceiling, the report list and the header leave the whole ring room.
inline constexpr std::size_t kPreviousSessionMaxLines = 80;
inline constexpr std::size_t kPreviousSessionMaxBytes = 12 * 1024;

// How many reports are listed, and the most that are ever examined to choose
// them (a folder nobody has ever cleared must not turn a click into a long walk).
inline constexpr std::size_t kListedReports = 10;
inline constexpr std::size_t kExaminedReports = 500;

// The most of one report file that is read. A freeze report writes its module list
// (up to 256 modules of about 190 bytes, so 48 KiB) BEFORE the `--- process ---`
// block that holds the uptime, so the bound has to clear that; a crash report's
// process block comes straight after its stack. A report that is longer than this
// is not read further and its uptime reads "unknown".
inline constexpr std::size_t kReportReadBytes = 128 * 1024;

// The most of one log file that is read (the live file rotates at 1 MiB; this
// is for a file somebody else made large, of which the newest end is read).
inline constexpr std::size_t kLogFileReadBytes = 2 * 1024 * 1024;

struct PreviousSessionLog {
    // True when the files held a session before the current one.
    bool found = false;
    // The last lines of that session, oldest first. UNSCRUBBED: the bundle's
    // builder passes them through core::scrubUploadLog.
    std::vector<std::string> lines;
    // "FoxSDR 0.99.59 (abc123def456)" - the session's own start line, which
    // names the build and nothing else; empty when it was not found.
    std::string build;
    // How many lines that session had in the files that were read.
    std::size_t sessionLines = 0;
    // Why there is none, in words the bundle prints.
    std::string reason;
};

// The log files in `logDir`, oldest first - foxsdr.2.log, foxsdr.1.log,
// foxsdr.log (DiagLog::kKeptFiles of them) - concatenated, cut into sessions at
// the `FoxSDR <version> (<commit>) starting` line that main() writes first
// (isSessionStartLine), and the last lines of the session BEFORE the last one.
// The last one is taken to be this session: it is the newest start line.
// An empty `logDir`, an unreadable directory and a directory with no start line
// all answer found = false with the reason.
PreviousSessionLog readPreviousSessionLog(const std::string& logDir,
                                          std::size_t maxLines = kPreviousSessionMaxLines,
                                          std::size_t maxBytes = kPreviousSessionMaxBytes);

// Whether `line` is a session's start line, as a log file holds it:
// "12:34:56.789 info FoxSDR 0.99.61 (abc123def456) starting".
bool isSessionStartLine(const std::string& line);

// THE END OF THE NEWEST SESSION'S LOG (0.99.64), for the sentinel: it writes the
// report of a session that has just ended and has no in-memory ring to copy, so
// it reads the same lines from the files the application flushed them to (every
// line is flushed as it is written). The same files, the same oldest-first
// order and the same session boundary as readPreviousSessionLog - but the NEWEST
// session, not the one before it: from the last start line to the end, and of
// that the last `maxLines`. UNSCRUBBED, like the ring a crash report copies; the
// uploader scrubs every line as it builds the request.
//
// If the files hold no start line (diagnostics was switched on part-way through
// the session) the whole of what was read is the tail, which can begin in an
// earlier session; `found` says which. Two instances share one log file, so
// their lines interleave: the tail is the end of that shared file.
struct SessionLogTail {
    bool found = false;               // a start line was found and the tail begins at it
    std::vector<std::string> lines;   // at most maxLines, oldest first
    std::size_t sessionLines = 0;     // how many lines the session had in the files
};
SessionLogTail readNewestSessionLogTail(const std::string& logDir, std::size_t maxLines);

// ONE REPORT, reduced to what is not identifying.
struct ReportSummary {
    std::string kind;       // "crash", "hang" or "stall"; "unknown" for a file that says none
    std::int64_t ageSec = -1;  // seconds since the file was written; -1 when unknown
    std::string version;    // the version that wrote it; "" when the file does not say
    std::int64_t uptimeSec = -1;     // the session's age at the fault; -1 when absent
    std::int64_t stalledMs = -1;     // a freeze: how long the interface was unresponsive
    std::string reason;     // a crash: its `reason` line, capped; "" for a freeze
    std::string code;       // a crash: its `code` line ("0xC0000005"); "" for a freeze
    std::string signature;  // the grouping signature
    std::string upload;     // sent, duplicate, local-only, backoff, rate-limited, failed,
                            // abandoned, too-large, expired, refused, other - or none
};

struct ReportListing {
    // False when the reports folder could not be read at all (as opposed to
    // being empty).
    bool readable = false;
    std::size_t total = 0;            // crash-* / hang-* files in the folder
    bool totalCapped = false;         // the folder had more entries than were examined
    std::vector<ReportSummary> newest;  // at most kListedReports, newest first
};

// The newest `maxListed` of the crash-*.txt / hang-*.txt files in `crashDir`
// (newest by the time they were written), each reduced to a ReportSummary, with
// `now` the epoch seconds the ages are measured from. Opens each chosen report
// for at most kReportReadBytes and its `.upload` sidecar; a `.dmp`, the
// diagnostics bundle and everything else in the folder is never opened.
ReportListing listRecentReports(const std::string& crashDir, std::time_t now,
                                std::size_t maxListed = kListedReports);

// One report as the bundle's line, no path in it:
//   crash, 12 min ago, version 0.99.59, uptime 3600 s, access violation (0xC0000005),
//   signature 1B2C3D4E5F607182, upload sent
//   hang, 2 h ago, version 0.99.59, uptime 1200 s, stalled 7213 ms, signature ..., upload none
std::string reportSummaryLine(const ReportSummary& r);
// "just now", "12 min ago", "3 h ago", "5 d ago"; "(age unknown)" for a negative age.
std::string agoText(std::int64_t ageSec);

// BOTH, as the bundle carries them. `included` false is the diagnostics-off case:
// the bundle adds neither section, not even a heading.
struct DiagHistory {
    bool included = false;
    // The reads have not finished (DiagHistoryCache before its first answer):
    // the bundle says so rather than claiming there is nothing to show.
    bool pending = false;
    PreviousSessionLog previous;
    ReportListing reports;
};

// Both reads, for a caller that can wait for them (a test, a tool).
DiagHistory readDiagHistory(const std::string& logDir, const std::string& crashDir,
                            std::time_t now);

// THE READS, OFF THE FRAME LOOP. The folder behind these reads is the one
// `__std_fs_get_stats` froze a window on in the 0.99.59 report (see
// docs/DIAGNOSTICS.md), and the problem-report page rebuilds its attachment about
// once a second while its box is ticked; so nothing on the GUI thread reads a
// file. A cache owns one worker at a time: refresh() starts a read when none is
// in flight and the answer is older than `maxAgeMs` (or none was ever taken),
// snapshot() returns the newest finished answer without waiting, and waitIdle()
// gives a read in flight a bounded time to finish - never a join. The previous
// session never changes, so it is read ONCE and carried across later refreshes;
// only the report listing is read again. At destruction a read still blocked in
// the filesystem is abandoned, not waited for: the worker owns what it touches.
class DiagHistoryCache {
public:
    DiagHistoryCache();
    ~DiagHistoryCache();
    DiagHistoryCache(const DiagHistoryCache&) = delete;
    DiagHistoryCache& operator=(const DiagHistoryCache&) = delete;

    // Starts a read unless one is in flight or the newest answer is younger than
    // `maxAgeMs`. `nowMs` is a monotonic clock in milliseconds.
    void refresh(const std::string& logDir, const std::string& crashDir, std::int64_t nowMs,
                 std::int64_t maxAgeMs);

    // The newest finished answer; `included` is false until the first read has
    // finished (and is set true by the cache for every answer it returns).
    DiagHistory snapshot() const;

    // True once a read has finished.
    bool ready() const;

    // Waits up to `limit` for a read in flight to finish; true when none is.
    bool waitIdle(std::chrono::milliseconds limit) const;

    // How many reads have been STARTED over this cache's life. The proof that
    // diagnostics-off reads nothing: it stays at zero.
    std::size_t readsStarted() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_DIAG_HISTORY_HPP
