// diag_log.hpp - the rotating application log, and the in-memory ring the
// crash and hang reports are flushed from.
//
// WHY A RING AS WELL AS A FILE. The state leading up to a fault identifies it
// far more often than the faulting frame does: "opened the B200, set 2.4 Msps,
// started the ADS-B plugin, CAT client connected" is a reproduction; a return
// address in ucrtbase is not. The file is what a user can find and send; the
// ring is what a report can carry WITHOUT touching the file, which matters
// because the file is written through the CRT (buffers, locks, heap) and a
// crash handler that touched any of those could deadlock or fault again.
//
// WHY THE RING IS FIXED STORAGE. Every line lives in a preallocated
// char[kRingLines][kLineBytes] block that exists from the first call onwards.
// Copying it out at crash time is a memcpy from memory that was never
// allocated on the failing heap, so it cannot fail because the heap is
// corrupt - which is precisely the state a crash handler runs in.
//
// WHY THE CRASH-PATH READER TAKES NO LOCK. copyRingRaw() deliberately does
// not acquire mutex_. A crash can happen while a *different* thread holds it,
// and a handler that blocked there would hang the process instead of
// reporting it - turning a crash into the one thing worse than a crash, a
// hang with no report. The cost is bounded and known: at most ONE line can be
// observed half-written, and it is the newest one. A torn last line is an
// acceptable price for a report that always arrives; a deadlock is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DIAG_LOG_HPP
#define CASCADE_CORE_DIAG_LOG_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::core {

// %LOCALAPPDATA%/FoxSDR on Windows (the same per-user tree the plugin
// directory already lives in - LOCALAPPDATA, not APPDATA, because none of
// this should follow a roaming profile onto another machine), the XDG state
// directory elsewhere. Empty only when the environment has no usable base,
// which disables every on-disk part of diagnostics rather than scattering
// files into the current directory.
std::string diagBaseDir();

// <diagBaseDir()>/crashes and <diagBaseDir()>/logs. Neither is created by
// these functions; the owner of the feature creates them once, on the healthy
// start-up path, so that no fault path ever has to.
std::string diagCrashDir();
std::string diagLogDir();

class DiagLog {
public:
    // 256 lines of 192 bytes is 48 KiB resident, and 256 lines is roughly the
    // last few minutes of a normal session's events (this log records state
    // changes, not frames). Both are compile-time constants so the ring is a
    // plain array with no allocation anywhere in its lifetime.
    static constexpr int kRingLines = 256;
    static constexpr int kLineBytes = 192;  // including the terminating NUL

    // Rotation. 1 MiB per file and three files kept: enough that a fault an
    // hour into a session still has its start-up in the log, bounded enough
    // that a user's profile never grows a log they did not ask for.
    //
    // kKeptFiles COUNTS THE LIVE FILE: 3 means foxsdr.log, foxsdr.1.log and
    // foxsdr.2.log - at most ~3 MiB, and never a foxsdr.3.log. The count is
    // asserted after three rotations in tests/test_diagnostics.cpp, because an
    // off-by-one in the shift loop keeps a fourth file and the first two
    // rotations look identical either way.
    static constexpr std::size_t kRotateBytes = 1024u * 1024u;
    static constexpr int kKeptFiles = 3;

    static DiagLog& instance();

    // `dir` empty or `enabled` false DISABLES the file entirely: no directory
    // is created, no file is opened, nothing is written to disk. The ring
    // keeps running either way - it never leaves the process unless the user
    // presses Copy diagnostics, so there is nothing to consent to, and a user
    // who turns diagnostics on mid-session should not have to reproduce the
    // fault before the log has anything in it.
    void configure(const std::string& dir, bool enabled);

    bool fileEnabled() const;
    std::string filePath() const;  // empty when the file is disabled

    void write(const char* level, const char* msg);

    // printf-style. Anything longer than kLineBytes-1 is TRUNCATED rather
    // than allocated for: a log line is a diagnostic, not a data channel.
    void writef(const char* level, const char* fmt, ...);

    // Oldest first, newest last, at most kRingLines entries. Normal-path
    // reader: takes the lock, allocates, and is used by Copy diagnostics.
    std::vector<std::string> ringSnapshot() const;

    // Total lines ever written, including the ones the ring has dropped.
    // A report that says "last 256 of 4011 lines" is honest about what it is
    // not carrying.
    std::uint64_t linesWritten() const;

    // CRASH-PATH READER. No lock, no allocation, no CRT: copies the ring into
    // `out` as NUL-terminated lines separated by '\n' and returns the number
    // of bytes written (never more than cap, always NUL-terminated when
    // cap > 0). See the header comment for the torn-line trade.
    std::size_t copyRingRaw(char* out, std::size_t cap) const;

    // Test hook: empties the ring and forgets the file. Never called by the
    // application.
    void resetForTest();

private:
    DiagLog() = default;

    void appendToFileLocked(const char* line, std::size_t len);
    void rotateIfNeededLocked(std::size_t incoming);

    mutable std::mutex mutex_;
    std::string dir_;
    std::string path_;
    bool enabled_ = false;
    std::FILE* fp_ = nullptr;
    std::size_t fileBytes_ = 0;

    // ATOMIC because copyRingRaw reads them WITHOUT the lock (see the header
    // comment). Plain ints here would be a data race in the letter as well as
    // the spirit, and the compiler would be within its rights to keep one of
    // them in a register across the crash handler's read. `next_` is the slot
    // the NEXT line goes into, so the oldest surviving line is at
    // next_ - min(written_, kRingLines).
    std::atomic<std::uint64_t> written_{0};
    std::atomic<int> next_{0};
    char ring_[kRingLines][kLineBytes] = {};
};

// Convenience wrappers so call sites read as prose. Every one of these is a
// no-op beyond the ring when the file is disabled.
void diagLogf(const char* fmt, ...);
void diagWarnf(const char* fmt, ...);

}  // namespace cascade::core

#endif  // CASCADE_CORE_DIAG_LOG_HPP
