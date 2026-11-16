// radio_reporter.hpp - the radio failure report, the radio journal and the first-session record: the on-disk queue, the daily bound and the
// sender (docs/radio-reports-contract.md, schema 2, sections 1 to 3 and 7).
//
// THE OWNER'S RULES THIS KEEPS (D4, D5, C2, C8, L7): every radio that does not open is its OWN report, never a count, sent whether the Diagnostics
// switch is on or off and whether Usage reporting is on or off; everything that happens to every physical radio is a journal event, sent while
// Usage reporting is on; one-time users are seen from inside their first session; nothing is lost to a crash (an event is on disk, flushed, BEFORE
// the call that could hang or die, and whatever was not sent goes at the next start); nothing is dropped without being counted and said.
//
// THE QUEUE is a folder `radio-reports` beside config.json: one file per radio failure (`fail-*.json`, which may need a degraded resend), and an
// append-only segment per session for the journal (`journal-<sessionId>.<n>.jsonl`, a header line then one complete message per line, rotated at
// 256 KiB, with a `.pos` cursor of the lines already taken). The folder is bounded at 200 failure files, 4 MiB and 14 days; over a bound the OLDEST
// is dropped and counted in the next event's `dropped`. The daily bound (100 radio_fail, 3,000 journal events; the first of each distinct
// (radio, class) or (radio, kind, word) of the day always passes; the newest later one is held and sent when the day rolls over, at the next start
// or at quit with `dropped` set) is kept in `state.json` beside them.
//
// THE SENDER: one message per request in order, at the start of the session, about every five minutes, at once on an urgent event, and at quit
// (bounded and abortable, quit is never delayed). A batch stops at the first 429, 5xx or network error. The literal answer `bad id` (the Worker
// deployed today) is "not ready": keep and retry, never delete. 400 `refused: <text-rule code>` is resent ONCE with the text withheld; any other
// refusal drops the message and counts it.
//
// THE SWITCH. kRadioReportsLiveSend (radio_report.hpp) is false in this build: the queue is written, nothing is posted.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_REPORTER_HPP
#define CASCADE_CORE_RADIO_REPORTER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/radio_report.hpp"

namespace cascade::core {
class UploadCancel;
}

namespace cascade::core::radioreport {

// What one POST said. `attempted` is false when the request never left (no network, cancelled).
struct PostResult {
    bool attempted = false;
    int status = 0;                       // 0 when no answer came
    std::uint64_t retryAfterSec = 0;      // from a 429
    std::string body;                     // the answer's text (a refusal's `refused: <code>`, or the old Worker's `bad id`)
};
using Transport = std::function<PostResult(const std::string& url, const std::string& json)>;

// The real transport: core::postBounded to the endpoint, abortable through `cancel`.
Transport makeRealTransport(const std::shared_ptr<UploadCancel>& cancel);

enum class Trigger { StartupRestore, UserPick, SavedRetry, PatchRadio, AutoReopen, Other };
const char* triggerWord(Trigger t);

// ---------------------------------------------------------------------------
// What the caller hands in
// ---------------------------------------------------------------------------

struct FailureInput {
    std::string driverKind;    // "soapy", "rtlsdr", ... (the request's kind, never the receiver's current device)
    std::string args;          // the device arguments (never sent: only the model and the serial are read from them)
    std::string nativeLabel;   // the scanned row's label when the caller has one; "" gives the family's plain name
    std::string reason;        // the driver's own sentence, UNSCRUBBED (the sender scrubs it)
    std::string reasonClass;   // health::radioReasonWord(classifyRadioOpen(kind, reason)), computed on the unscrubbed text
    Trigger trigger = Trigger::Other;
    bool superseded = false;   // the user had already moved on to another source when the result arrived
    long long openMs = -1;     // how long the open took before it failed; -1 unknown
    long long sampleRateSps = -1;   // the rate asked for, in samples per second; -1 unknown
    bool soundCard = false;    // the sound-card input: model "sound card", no serial
};

using FieldValue = std::variant<std::string, long long, double, bool, std::vector<std::string>, std::vector<long long>>;

// One journal event: a kind of contract section 3, the radio it is about (empty for session_env), the kind's own fields. TEXT fields are
// passed through `text()` so they are scrubbed on the way in; nothing else is free text.
struct Event {
    std::string kind;                  // radio_found, radio_open, radio_settings, radio_stream, radio_recovery, radio_close, session_end, session_env
    RadioIdentity radio;               // empty key for session_env
    std::string word;                  // the phase / cause / why word that makes the daily bound's (radio, kind, word) triple
    std::vector<std::pair<std::string, FieldValue>> fields;
    bool urgent = false;               // send at once: an unclean close, a recovery that gave up
    long long atSec = -1;              // seconds since the session started, or -1 for "now"

    Event& set(const std::string& name, const std::string& v) { fields.emplace_back(name, FieldValue(v)); return *this; }
    Event& set(const std::string& name, const char* v) { fields.emplace_back(name, FieldValue(std::string(v))); return *this; }
    Event& set(const std::string& name, long long v) { fields.emplace_back(name, FieldValue(v)); return *this; }
    Event& set(const std::string& name, int v) { fields.emplace_back(name, FieldValue(static_cast<long long>(v))); return *this; }
    Event& set(const std::string& name, double v) { fields.emplace_back(name, FieldValue(v)); return *this; }
    Event& set(const std::string& name, bool v) { fields.emplace_back(name, FieldValue(v)); return *this; }
    Event& set(const std::string& name, const std::vector<std::string>& v) { fields.emplace_back(name, FieldValue(v)); return *this; }
    Event& set(const std::string& name, const std::vector<long long>& v) { fields.emplace_back(name, FieldValue(v)); return *this; }
    // Free text: scrubbed (serial, paths, addresses, frequencies) and clipped to `maxChars` before it is kept.
    Event& text(const std::string& name, const std::string& raw, std::size_t maxChars);
};

// Who is sending, and what is true of this install right now.
struct Identity {
    std::string installId;        // empty: Usage reporting is off (only radio failures are sent, anonymously)
    std::string version, os, arch, channel, updateChannel;
    std::string window;           // "new", "legacy" or ""
    bool diagOn = false;          // the Diagnostics switch
    std::uint64_t launches = 0;   // launches since install (the first-session record needs 1)
};

struct Stats {
    std::uint64_t failuresQueued = 0, journalQueued = 0;
    std::uint64_t sent = 0;               // accepted by the server
    std::uint64_t posts = 0;              // requests made
    std::uint64_t refused = 0;            // refused for good and dropped, counted
    std::uint64_t degradedResends = 0;    // sent a second time with the text withheld
    std::uint64_t notReady = 0;           // answered `bad id`: kept
    std::uint64_t expired = 0;            // older than 14 days
    std::uint64_t overflowDropped = 0;    // dropped by the folder's bounds
    std::uint64_t boundHeld = 0;          // held by the daily bound (sent later, counted in `dropped`)
    std::uint64_t boundDropped = 0;       // not sent because a later one replaced them (counted in `dropped`)
    std::uint64_t guardRefused = 0;       // refused locally: a forbidden field or text that would not pass
    std::uint64_t suppressedByBuild = 0;  // batches that did nothing because kRadioReportsLiveSend is false
    std::size_t failFilesOnDisk = 0, journalLinesPending = 0;
};

class Reporter {
public:
    struct Options {
        std::string dir;                      // the `radio-reports` folder; empty disables everything (a hermetic run)
        std::string endpoint;                 // telemetryEndpoint(); empty disables everything
        std::function<std::int64_t()> nowEpoch;   // UTC seconds; default the system clock
        Transport transport;                  // default: the real one
        bool liveSend = kRadioReportsLiveSend;
        Identity identity;                    // who is sending (set BEFORE the queue is opened: what an earlier day held is released at configure)
        std::string sessionId;                // default: 16 random hex characters
        std::int64_t sessionStartEpoch = 0;   // default: now
        std::size_t maxFailFiles = 200;
        std::size_t maxFolderBytes = 4u * 1024u * 1024u;
        std::size_t segmentBytes = 256u * 1024u;
        std::int64_t maxAgeSec = 14 * 24 * 3600;
        int failuresPerDay = 100;
        int journalPerDay = 3000;
        int streamWidenAfter = 500;           // stream events a radio sends in a day before its window widens to 5 minutes
        int maxBatch = 50;
        std::int64_t batchEverySec = 300;
    };

    Reporter();
    ~Reporter();
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    // Opens the queue (creates the folder, reads the day's state, releases what an earlier day held). Safe to call again with new options.
    void configure(const Options& options);
    bool active() const;

    // Who is sending. Turning Usage reporting OFF (installId empty) deletes every queued message that carries an install id and re-issues queued
    // identified failures anonymously; nothing of the old identity stays on disk.
    void setIdentity(const Identity& id);
    Identity identity() const;
    // The Diagnostics switch as it is now: it changes nothing that is sent, it is a field of every failure report (`diagOn`).
    void setDiagOn(bool on);
    // Arms the live send (an interactive run does it once the window is up; a bounded or hermetic run never does). Never turns on what
    // kRadioReportsLiveSend leaves off.
    void setLiveSend(bool on);
    // The clock used for what is kept before configure() (default: the system clock). For the tests.
    void setNowSource(std::function<std::int64_t()> now);

    // ONE report for one radio that did not open, EVERY time (never folded, never skipped for a cap). Sent with the Diagnostics switch on or off
    // and with Usage reporting on or off (without an install id the message is the anonymous form of contract 1.2).
    void fileFailure(const FailureInput& in, const std::vector<std::string>& logRing);

    // One journal event. Ignored without an install id (Usage reporting off: there is no anonymous journal).
    void event(const Event& ev);

    // The first-session record (contract 7): `usageJson` is TelemetryReport::toJson() of the session so far. Sent only in the install's first
    // session (launches == 1), once about a minute in and again on a failure, at most one a minute and 6 a session. It never touches the ledgers.
    void firstSession(const std::string& usageJson, const std::string& trigger);

    // The keys of the radios open right now (for the beat). The engine sets it when a radio opens or closes.
    void setOpenRadios(const std::vector<std::string>& keys);
    std::vector<std::string> openRadios() const;

    // Seconds the stream sampler should cover for this radio's next radio_stream event: 60, or 300 after 500 events in a day (contract 3.9).
    int streamWindowSec(const std::string& radioKey) const;

    // Called often (the engine's frame): starts a batch when one is due; reaps a finished one. Never blocks.
    void poll();
    // Releases what the daily bound held and tries one last bounded send; returns after `maxMs` at the latest. Quit is never delayed by a stuck send.
    void flushAtQuit(int maxMs);
    // Runs one batch on the calling thread (tests, and flushAtQuit). Returns the number of requests accepted.
    int runBatchNow();
    // Joins the sender thread (cancelling a transfer in flight).
    void shutdown();

    Stats stats() const;
    std::string sessionId() const;
    std::int64_t nowEpoch() const;

    // For the tests: the held messages, the state file's path and the folder.
    std::string dir() const;
    std::size_t heldCount() const;

private:
    struct Impl;
    std::shared_ptr<Impl> p_;   // shared so that a sender thread still in flight at exit owns what it uses
};

// CALL SITES THAT HAVE NO ENGINE (a worker thread, the window): each builds its event and hands it to global(); they take no lock of their own and may be
// called from any thread. `binding` and `source` are the contract's words (3.2); `usbId` is "vvvv:pppp" or empty.
void noteFound(const std::string& driverKind, const std::string& args, const std::string& label, const std::string& binding, const std::string& source,
               const std::string& usbId = std::string());
// A recovery of a radio (contract 3.6): `phase` is "start", "ok" or "gave-up"; `what` is the application's own word ("reopen", "routefallback", ...).
void noteRecovery(const std::string& driverKind, const std::string& args, const char* phase, const std::string& what, const std::string& reason = std::string(),
                  long long ms = -1);

// THE PROCESS-WIDE REPORTER the engine's call sites use. Unconfigured until the engine configures it at start-up; an unconfigured reporter
// ignores every call (a hermetic or bounded run queues and sends nothing).
Reporter& global();

// The text of a failure or event: used by the tests that scan the queue.
std::vector<std::string> readQueueFiles(const std::string& dir);

}  // namespace cascade::core::radioreport

#endif  // CASCADE_CORE_RADIO_REPORTER_HPP
