// crash_upload.hpp - getting a captured report to the people who can fix it,
// without making the product worse to use.
//
// Phase 1 (0.61.0) captures a crash or a freeze to disk and stops there. A
// report nobody ever sees is a diary, not a diagnostic: the whole value of the
// feature is that a fault on a machine we will never touch reaches a build
// engineer. This file is the half that carries it.
//
// ---------------------------------------------------------------------------
// THE FOUR RULES THAT OUTRANK THE FEATURE
// ---------------------------------------------------------------------------
//
// 1. NEVER UPLOAD FROM INSIDE THE FAULT HANDLER. The process that is running
//    the handler has already failed once; it cannot safely allocate, take a
//    lock, or open a socket, and WinHttpOpen does all three. Phase 1 writes to
//    disk from the handler ON PURPOSE. The upload happens on the NEXT START -
//    a healthy process, with a heap, a message loop and a user who can be
//    asked. `telemetryCleanExit` already knows the last run ended badly, and
//    the reports are on disk waiting, so nothing has to survive the crash
//    except the file.
//
// 2. NEVER BLOCK OR DELAY THE APPLICATION, INCLUDING SHUTDOWN. A report is
//    worth nothing if collecting it makes the product worse. The sweep runs on
//    a background thread started after the window is up; every transfer is
//    bounded by short WinHTTP timeouts; and the thread is CANCELLABLE - the
//    request handle is published to an atomic before the blocking calls, and
//    cancel() closes it, which makes a blocked WinHttpReceiveResponse return
//    ERROR_WINHTTP_OPERATION_CANCELLED immediately instead of sitting out its
//    timeout. That is the difference between "shutdown is delayed by seconds
//    against a server that accepts and never answers" and "shutdown is not
//    delayed at all", and it is measured against the real binary in
//    tests/test_crash_upload.cpp rather than asserted here.
//
// 3. RATE-LIMIT AND DEDUPLICATE ON THE CLIENT. A machine in a crash loop must
//    stop sending before the server has to say 429 - by then the traffic has
//    already been paid for by both sides, and a user in a boot loop would send
//    the same stack a hundred times. Two independent limits, both persisted in
//    the config so they survive the restart that a crash loop consists of:
//    the same SIGNATURE is sent at most once per kDedupSeconds, and at most
//    kMaxPerWindow reports are sent per kWindowSeconds whatever their
//    signatures. A 429 that does arrive anyway is honoured - see
//    noteRateLimited().
//
// 4. NOTHING IS SILENTLY LOST, AND NOTHING IS RETRIED FOR EVER. Every report
//    the sweep looks at gets a "<report>.upload" sidecar recording what
//    happened to it and how many attempts it has had. A transfer that fails is
//    retried on later starts up to kMaxAttempts, then marked `abandoned` - the
//    text report stays exactly where it is, so "Open reports folder" and "Copy
//    diagnostics" still work and the user can still send it by hand. There is
//    no state anywhere that can make a report disappear.
//
// ---------------------------------------------------------------------------
// WHAT IS SENT, AND WHAT IS NOT
// ---------------------------------------------------------------------------
//
// Settled by the owner, 2026-08-25, and this list is the one PRIVACY.md
// documents field by field:
//
//   AUTOMATIC, when the Diagnostics switch is on: version, commit, build id,
//   OS and architecture, the faulting module and offset, the grouping
//   signature, every thread's stack AS MODULE+OFFSET, the recent log ring, and
//   the application context (mode, source, sample rate, device state, SDR
//   model with the serial stripped, plugins with their versions).
//
//   NEVER, under any setting: the MINIDUMP. It is process memory, which on
//   this application can include file paths, window titles and captured I/Q.
//   It is written locally when the user asks for it and offered for manual
//   sending. sweepCrashDir() opens files named crash-*.txt and hang-*.txt and
//   nothing else, and tests/test_crash_upload.cpp proves a .dmp sitting beside
//   a report never appears in a request body.
//
// OFF MEANS OFF FOR UPLOADING TOO. The existing Settings switch already means
// "no directory, no report, no minidump". It now also means no upload: with it
// off the sweep is never started, no sidecar is written, and no socket is
// opened. That is asserted against the real binary, not just here.
//
// A note on the install id: the payload carries the SAME anonymous id the
// usage report uses, because the server has to be able to rate-limit one
// machine without knowing anything about it. When usage reporting is off that
// id does not exist (it is deleted, see telemetry.hpp), and the field is sent
// empty rather than minted - a crash report must not be the thing that creates
// an identifier the user turned off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CRASH_UPLOAD_HPP
#define CASCADE_CORE_CRASH_UPLOAD_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace cascade::core {

// ---------------------------------------------------------------------------
// The report, parsed back off disk
// ---------------------------------------------------------------------------
//
// The uploader reads the SAME FILE the fault handler wrote. It does not keep a
// second in-memory copy of the fault, because the fault happened in a process
// that no longer exists - and a second representation would be a second thing
// to keep in step with crash_handler.cpp and hang_watchdog.cpp.
struct ReportFrame {
    std::string module;   // "cascade.exe", or empty when the address resolved
                          // to no module the snapshot knew about
    std::string buildId;  // from the report's own --- modules --- block
    std::uint64_t offset = 0;
    std::uint64_t rawAddress = 0;  // set only when `module` is empty
};

struct ReportThread {
    std::uint64_t id = 0;
    std::vector<ReportFrame> frames;
};

struct ParsedReport {
    std::string kind;  // "crash" | "hang"; anything else is refused
    std::string signature;
    std::string version;
    std::string commit;
    std::string os;
    std::string arch;

    // The faulting module, from the crash header's `address:` line or from the
    // first frame of the stalled thread in a hang report.
    std::string module;
    std::string buildId;
    std::uint64_t offset = 0;

    // Context, exactly the fields the shared context block carries that are
    // not already top-level.
    std::string mode;
    std::string source;
    std::string sdrModel;
    double sampleRateHz = 0.0;
    bool deviceOpen = false;
    std::vector<std::string> plugins;  // "name version", as the report writes them

    // The report's own --- modules --- block: file name -> build id, with an
    // empty build id where the report said "(none)". This is what puts a build
    // id on every frame, and it is kept rather than folded away because it is
    // also the only thing that can attach one to a PLUGIN entry.
    std::vector<std::pair<std::string, std::string>> modules;

    std::vector<ReportThread> threads;
    std::vector<std::string> log;
};

// Parses the text a report writer produced. False when the text is not a
// report at all (no `kind:` line, or a kind this build does not know), which
// is how a truncated or hand-edited file is refused rather than posted as
// nonsense the server has to reject.
bool parseReportText(const std::string& text, ParsedReport& out);

// ---------------------------------------------------------------------------
// The wire payload
// ---------------------------------------------------------------------------
//
// THE CONTRACT, agreed with the receiving end so neither side waits for the
// other (2026-08-25):
//
//   POST https://foxsdr.com/api/crash    Content-Type: application/json
//   { schema:1, kind, version, commit, buildId, module, offset, signature,
//     os, arch, installId, plugins:[{name,version,buildId}], context:{...},
//     log:[lines], threads:[{id,frames:[{module,buildId,offset}]}] }
//   204 accepted | 400 malformed | 413 too large | 429 rate limited
//
//   NO AUTHENTICATION. Reports are anonymous by design, so the defence is size
//   caps and rate limiting - never a secret compiled into every shipped
//   binary, which is not a secret at all.
//
// `offset` is a JSON NUMBER of bytes, here and inside every frame. It is an
// offset within a module, so it is small; rendering it as hex would have been
// one more thing for two independently written parsers to disagree about.
std::string uploadJson(const ParsedReport& r, const std::string& installId);

// THE FIELD INVENTORY, in the same spirit as TelemetryReport's and the
// bundle's: PRIVACY.md documents the uploaded payload field by field, and
// tests/test_crash_upload.cpp compares these lists with what uploadJson()
// actually emits IN BOTH DIRECTIONS. A field added to the payload fails just
// as loudly as a field this list claims and the payload stopped emitting.
const std::vector<std::string>& uploadFieldNames();
const std::vector<std::string>& uploadContextFieldNames();

// The hard ceiling on one request body. The server answers 413 above its own
// limit; a client that made the server enforce that would be spending both
// sides' bandwidth to learn something it already knew. The log is trimmed
// first, then the thread stacks, and a payload that is still too large after
// both is refused locally and marked `too-large` rather than sent.
constexpr std::size_t kMaxUploadBytes = 128 * 1024;

// ---------------------------------------------------------------------------
// Client-side rate limiting and deduplication
// ---------------------------------------------------------------------------
constexpr std::uint64_t kDedupSeconds = 24 * 60 * 60;
constexpr std::uint64_t kWindowSeconds = 24 * 60 * 60;
constexpr int kMaxPerWindow = 5;
constexpr int kMaxAttempts = 3;
// Used when a 429 arrives with no Retry-After, and as the ceiling for one that
// asks for something absurd.
constexpr std::uint64_t kDefaultBackoffSeconds = 60 * 60;
constexpr std::uint64_t kMaxBackoffSeconds = 24 * 60 * 60;
// How many signatures the dedup memory holds. Small on purpose: it lives in
// the user's config file, and a machine with more than this many DISTINCT
// faults in a day has a problem that one more report will not clarify.
constexpr std::size_t kMaxRecentSignatures = 32;

// Persisted across runs, because a crash loop IS a sequence of runs - a limit
// held only in memory would reset on every restart and limit nothing.
struct UploadPolicyState {
    std::vector<std::string> recent;  // "<signature> <epoch seconds>"
    std::uint64_t windowStart = 0;
    std::uint32_t windowCount = 0;
    std::uint64_t blockedUntil = 0;  // epoch seconds; set by a 429
};

enum class UploadDecision {
    Send,
    Disabled,     // the Settings switch is off, or there is nowhere to send
    Duplicate,    // this signature was already sent inside kDedupSeconds
    RateLimited,  // kMaxPerWindow already used in this window
    Backoff,      // the server said 429 and the backoff has not expired
};

// Pure decision, no clock of its own and no I/O, so a test can drive a day of
// crash-looping in a few microseconds.
UploadDecision decideUpload(const UploadPolicyState& state, bool enabled,
                            const std::string& signature, std::uint64_t nowEpoch);

// Records a successful send: the signature joins the dedup memory and the
// window count advances.
void noteSent(UploadPolicyState& state, const std::string& signature, std::uint64_t nowEpoch);

// Honours a 429. `retryAfterSeconds` is the header's value, 0 when it was
// absent or unparsable; the result is clamped into
// [kDefaultBackoffSeconds/60, kMaxBackoffSeconds] so a hostile or broken
// header can neither disable the limit nor mute the client for a year.
void noteRateLimited(UploadPolicyState& state, std::uint64_t nowEpoch,
                     std::uint64_t retryAfterSeconds);

// The state as it is stored in (and read back from) AppConfig. Bounded and
// validated on the way in: this is user-editable text on disk.
std::vector<std::string> encodePolicyRecent(const UploadPolicyState& state);
UploadPolicyState decodePolicyState(const std::vector<std::string>& recent,
                                    std::uint64_t windowStart, std::uint32_t windowCount,
                                    std::uint64_t blockedUntil);

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
struct UploadResult {
    bool attempted = false;
    bool accepted = false;   // the server said 204 (or any 2xx)
    int status = 0;          // 0 when the request never got an answer
    bool rateLimited = false;
    std::uint64_t retryAfterSeconds = 0;
    bool cancelled = false;
};

// The cancel token. Shared with the worker thread by shared_ptr, so cancelling
// is safe even if the owner is being destroyed: the worker holds its own
// reference and the token outlives whoever calls cancel().
//
// `request` is the live WinHTTP request handle, published by the worker before
// it makes a blocking call and taken by whichever of the two gets there first,
// so it is closed exactly once. Closing it is what aborts a blocked transfer.
class UploadCancel {
public:
    void cancel();
    bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

    // Called by the transport only.
    bool publish(void* handle);  // false when cancel() already happened
    void* take();                // the handle, or nullptr if already taken

private:
    std::atomic<bool> cancelled_{false};
    std::atomic<void*> request_{nullptr};
};

// One POST. Synchronous, bounded, and abortable through `cancel`. Returns what
// the server said so the caller can honour a 429; unlike the usage reporter,
// which deliberately ignores the response, this one has to read the status -
// a rate limit that is not read is not honoured.
UploadResult postCrashReport(const std::string& url, const std::string& json,
                             const std::shared_ptr<UploadCancel>& cancel);

// WHERE REPORTS GO. https://foxsdr.com/api/crash, overridable by
// FOXSDR_CRASH_URL, which is how the tests point at a local stub with no
// network. Plain http is refused EXCEPT for a loopback host, so a test can use
// a socket while a shipped binary can never be talked into sending a report in
// clear over somebody's network.
std::string crashUploadEndpoint();

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------
struct SweepParams {
    std::string crashDir;
    std::string url;
    std::string installId;
    bool enabled = false;   // the Settings switch. False means do nothing at all.
    std::uint64_t nowEpoch = 0;
    UploadPolicyState state;
    // Bounded work per start: the newest few reports only. A directory with
    // 400 files in it is a machine that has been crashing for a month, and
    // sending all 400 on the next successful start would be the client
    // becoming the denial of service.
    int maxReports = 4;
};

struct SweepOutcome {
    int considered = 0;
    int sent = 0;
    int duplicate = 0;
    int limited = 0;   // client rate limit or server backoff
    int failed = 0;    // will be retried on a later start
    int abandoned = 0; // out of attempts; the file stays on disk
    int refused = 0;   // unparsable or too large
    UploadPolicyState state;
    std::vector<std::string> notes;  // one line per report, for the log
};

// Reads the crash directory, decides, posts, and writes each report's sidecar.
// Synchronous: CrashUploader runs it on a thread. Returns immediately with an
// empty outcome when `enabled` is false - off means off.
SweepOutcome sweepCrashDir(const SweepParams& params,
                           const std::shared_ptr<UploadCancel>& cancel);

// The sidecar a swept report carries. Exposed so a test can read it back and
// so the reader tool can explain a report that is still on a machine.
std::string uploadSidecarPath(const std::string& reportPath);

// Fire-and-forget owner of the sweep thread. NOT detached: the destructor
// cancels and then joins, because a detached thread inside WinHTTP while the
// process tears down is how a clean exit becomes a crash on exit - and a crash
// on exit would be counted, by this very feature, as a crash. The cancel is
// what keeps that join instant.
class CrashUploader {
public:
    CrashUploader() = default;
    ~CrashUploader();

    CrashUploader(const CrashUploader&) = delete;
    CrashUploader& operator=(const CrashUploader&) = delete;

    // No-op when a sweep is already running, or when params.enabled is false.
    void start(const SweepParams& params);

    // Cancels an in-flight transfer and joins. Safe to call more than once,
    // safe to call when nothing is running.
    void stop();

    bool busy() const;

    // The last finished sweep's outcome. Only read after stop().
    SweepOutcome outcome() const { return outcome_; }

private:
    std::thread thread_;
    std::shared_ptr<UploadCancel> cancel_;
    SweepOutcome outcome_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_CRASH_UPLOAD_HPP
