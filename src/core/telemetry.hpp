// telemetry.hpp - anonymous, opt-in usage reporting.
//
// WHAT THIS IS FOR. Product decisions that are currently guesses: whether
// anyone runs this on Linux, whether people update, which decoders justify
// further work, which radios to prioritise, and how often it falls over. All
// of those are answerable from aggregate counts, and none of them need to know
// anything about a person.
//
// WHAT IT DELIBERATELY DOES NOT COLLECT, and these are design constraints
// rather than an oversight:
//
//   - NO frequencies. What somebody listens to is the most sensitive thing
//     this application knows, it is a criminal matter in some jurisdictions
//     (see the POCSAG and Inmarsat-C notices), and it would tell us nothing
//     we could act on.
//   - NO decoded content, ever. Uploading intercepted pager or satellite
//     traffic would make the user the discloser and us the recipient of it.
//   - NO position, of the receiver or of anything it hears.
//   - NO IP address or derived location is recorded. A request necessarily
//     carries an IP to the edge; nothing stores it.
//   - NO device serial numbers. The SDR MODEL is useful ("B200"); the serial
//     is a unique hardware identifier and is stripped - see sanitiseDevice().
//
// CONSENT. Reporting is ON by default and the user turns it off (owner's
// decision, 2026-08-18; see the note on AppConfig::telemetryEnabled). The
// install id is minted on the first run that finds reporting enabled and no
// id, and DELETED when reporting is switched off, so a later opt-in cannot be
// linked to an earlier one.
//
// The risk that carries is recorded rather than hidden: PECR regulation 6
// requires consent before storing an identifier on someone's device for
// analytics, and an opt-out default is not consent. The owner has accepted
// that knowingly. PRIVACY.md, README.md and the website all state the default
// in the same words, and this comment is part of that set - it said the
// opposite for a while, which is worse than saying nothing, because an audit
// would have believed it.
//
// WHEN IT SENDS. At STARTUP, reporting the session that has already finished,
// never at exit. A network call on the shutdown path can hang the application
// while the user is trying to close it; and a report written to disk at exit
// would be lost by the one event most worth counting, a crash. Instead the
// session summary is journalled locally, and a clean exit is recorded - so a
// startup that finds the previous session unfinished knows it crashed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TELEMETRY_HPP
#define CASCADE_CORE_TELEMETRY_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/health_events.hpp"

namespace cascade::core {

// One session's worth of counters, accumulated in memory and journalled to
// the config at exit. Everything here is a count or a coarse label.
struct TelemetrySession {
    std::uint64_t seconds = 0;                  // how long the session ran
    std::map<std::string, std::uint64_t> modeSeconds;  // "WFM" -> seconds
    std::vector<std::string> panels;            // panels opened, deduplicated
    std::vector<std::string> plugins;           // "name version", installed
    std::string sdrModel;                       // "B200"; serial stripped
};

// What is actually transmitted. Kept as a struct so a test can assert on the
// exact set of fields - the privacy promise is only as good as the payload.
struct TelemetryReport {
    std::string installId;
    std::string appVersion;
    std::string os;         // "Windows 10.0.22631"
    std::string arch;       // "x64"
    std::uint64_t launches = 0;
    std::uint64_t crashes = 0;
    // DISPLAY STALLS: how many times the hang watchdog classified a freeze as
    // `kind: stall` (the display driver, not this application - see
    // HangWatchdog::isDisplayPresentationStall) and the record has not yet
    // been accepted by the server. A bare count: no stack, no module, no
    // driver name, no time of day. Those reports never leave the machine, so
    // without this number nobody can tell a user whose window freezes every
    // few minutes from one whose never has. See StallLedger below for where
    // the running count lives and why it survives a session killed mid-freeze.
    std::uint64_t stalls = 0;
    // FAILURES THAT ARE NOT CRASHES (0.99.64): a radio that would not open, no
    // sound output, an update or a plugin install that failed - counted, never
    // described. One string of `token=count` pairs from a fixed vocabulary
    // (core/health_events.hpp), e.g. "radio_fail.rtlsdr.busy=2,sound_ok.wasapi=1";
    // empty when there was nothing to report. toJson() passes it through
    // health::sanitise(), so nothing outside the vocabulary can be sent whatever
    // this member holds. Where the running counts live, and how they survive a
    // crash and are taken off only when a record carrying them was accepted:
    // health::HealthLedger, which does for these what StallLedger does for
    // `stalls`.
    std::string health;
    // WHERE THIS COPY CAME FROM and WHEN it first reported (0.99.47). The
    // channel is read from the running copy (installChannel()); the first-run
    // date and version are written once, when the install id is created, and
    // deleted with it. Empty for installs older than that - never guessed.
    std::string channel;       // "store", "installer", "appimage", "tarball", "android"
    std::string firstRun;      // "2026-09-29", UTC day only
    std::string firstVersion;  // "0.99.47"
    TelemetrySession session;

    // Compact JSON, exactly the fields above and nothing else.
    std::string toJson() const;
};

// Whole seconds banked from a clock sampled at ARBITRARY intervals.
//
// The caller samples once per rendered frame, so every individual delta is a
// fraction of a second. Truncating each delta on its own discards all of
// them, which is how mode-seconds reported zero for a whole release. Here the
// remainder is CARRIED instead: the mark advances only by the whole seconds
// actually banked, so 60 frames of 16.7 ms bank one second between them.
class SecondAccrual {
public:
    // Starts (or restarts) accrual at `now`. Nothing is banked, and any
    // remainder carried from before is dropped with the old mark.
    void reset(double now);

    // Whole seconds earned since the last call; the sub-second remainder is
    // kept for the next one. Zero before the first reset, and zero for a
    // clock that did not move forward.
    std::uint64_t advance(double now);

private:
    double mark_ = 0.0;
    bool started_ = false;
};

// A device string as SoapySDR reports it, reduced to something that
// identifies the MODEL and nothing else.
//
// "driver=uhd, label=B200 EDR04ZDB2, product=B200, serial=EDR04ZDB2, type=b200"
//   -> "uhd b200"
//
// The serial is a unique per-unit identifier - it would let two reports be
// tied to the same physical radio, which is precisely what anonymity means
// not doing. Anything not on the allowed key list is dropped, so a driver
// that invents a new key cannot leak it by default.
std::string sanitiseDevice(const std::string& soapyArgs);

// A fresh random install id: 32 lowercase hex characters from the system CSPRNG.
// Random, never derived from anything about the machine or the user - a
// hash of a MAC address or a hostname would be a fingerprint wearing a
// disguise. Empty on failure, which suppresses reporting rather than
// falling back to something guessable.
std::string newInstallId();

// True when `id` is exactly the shape newInstallId produces. Used to refuse a
// hand-edited config that tried to put something meaningful in the field.
bool validInstallId(const std::string& id);

// "Windows 10.0.22631" - the OS and its build, no machine or user name.
std::string osDescription();
std::string archDescription();

// WHERE REPORTS GO. Empty in this build, which DISABLES reporting entirely:
// with nowhere to send, the setting is unavailable rather than silently
// collecting. Set it to the deployed Cloudflare Worker (a route on the
// project's own domain rather than a workers.dev address, so the endpoint can
// move without orphaning binaries already installed).
//
// FOXSDR_TELEMETRY_URL overrides it, which is how the tests point at a local
// stub without a network.
std::string telemetryEndpoint();

// THE PRIVACY POLICY, AS A PAGE THE USER CAN OPEN FROM INSIDE THE APPLICATION.
// The Usage reporting panel already lists both payloads field by field, but a
// store listing (the Microsoft Store's developer agreement, and any reviewer
// reading it) requires a prominent link to the policy itself, from within the
// app, at the same address the listing names. One constant, so the panel, the
// listing and the site cannot drift apart; https because the page describes
// what leaves the machine and must not itself be tampered with on the way.
constexpr char kPrivacyPolicyUrl[] = "https://foxsdr.com/privacy.html";

// THE SITE ITSELF, for the one place the app has to hand someone to the web
// rather than doing something itself - Linux's update banner (app_window.cpp
// drawUpdateBanner()), which has no installer to run yet and says so.
constexpr char kHomepageUrl[] = "https://foxsdr.com";

// Posts one report, on a thread of its own, and forgets about it.
//
// Fire-and-forget, but NOT detached: the destructor joins, because a detached
// thread writing into a WinHTTP handle while the process tears down is how a
// clean exit turns into a crash on exit - and a crash on exit would be
// counted, by this very feature, as a crash. Transfers are bounded by short
// timeouts so the join cannot hold shutdown open.
//
// Every failure is silent. A usage counter that interrupted somebody's
// listening to complain it could not reach a server would be worse than
// having no usage counter.
//
// WHETHER THE SERVER TOOK IT. The transport reads the HTTP STATUS and nothing
// else from the answer - never the body - and `done` is told whether it was a
// 2xx. That is all a local counter needs to know to decide whether to keep
// itself (see StallLedger): the server still cannot instruct this client to do
// anything, only be heard or not. `done` runs on the sender thread, once, and
// is NOT called at all when nothing was sent (empty url or body, or a send
// already running) - "not sent" and "sent and refused" are both "not accepted"
// to a counter that only subtracts on an acceptance.
class TelemetryReporter {
public:
    // True when the server accepted the record (an HTTP 2xx).
    using SendDone = std::function<void(bool accepted)>;
    // One POST. True only for an accepted answer; every failure is false.
    using Transport = std::function<bool(const std::string& url, const std::string& json)>;

    TelemetryReporter() = default;
    ~TelemetryReporter();

    TelemetryReporter(const TelemetryReporter&) = delete;
    TelemetryReporter& operator=(const TelemetryReporter&) = delete;

    // No-op when `url` or `json` is empty, or when a send is already running.
    void send(const std::string& url, const std::string& json, SendDone done = SendDone());

    // The same send over a transport the CALLER supplies. send() is exactly
    // this with the real one; it exists so the "accepted" and "refused" halves
    // can be driven from a test without a TLS server on every platform.
    void sendVia(Transport transport, const std::string& url, const std::string& json,
                 SendDone done = SendDone());

    bool busy() const;

private:
    std::thread thread_;
};

// An HTTP answer this client counts as "the server took the record": 2xx.
bool httpStatusAccepted(int status);

// DISPLAY STALLS - the one number a stall report cannot give by itself.
//
// WHY A NUMBER AT ALL. A freeze report whose `kind` is `stall` (the display
// driver was waiting, not this application) is deliberately kept on the user's
// machine: a display driver's behaviour is not this product's fault to file.
// That is the right privacy and noise decision, and it means nobody can see
// HOW OFTEN it happens - a user reporting "the window freezes" may be hitting
// one every few minutes, and from outside that is indistinguishable from
// never. So the count, and only the count, rides in the usage record
// (TelemetryReport::stalls).
//
// WHERE THE RUNNING COUNT LIVES, AND WHY. The path a stall is most likely on is
// the user ending a frozen window from the taskbar, which is TerminateProcess:
// nothing runs, the GUI thread never saves the config again, and anything held
// only in memory or only in config.json is gone. The GUI thread is also the
// one thread that must never touch the disk during a freeze. So the count is
// written by the WATCHDOG's own thread, the instant the freeze is classified,
// to a tiny file beside config.json (`telemetry-stalls`): the install id it
// belongs to, a space, and the number. Nothing on the GUI thread's frame path
// reads or writes it - the frame loop only reads an atomic.
//
// WHEN IT IS SENT, AND WHEN IT RESETS. The record for a session is sent at the
// NEXT start (see the file header), so at that start the pending record is sent
// with `stalls` set to whatever the ledger holds, and the ledger is reduced by
// exactly that many ONLY WHEN THE SERVER ACCEPTED THE RECORD. A send that fails
// (no network, a refusal, a record that was never sent) leaves the count where
// it is and it rides the next record; a stall that happens after the record was
// built is not lost to the subtraction. The one way to count a stall twice is a
// record the server accepted whose acceptance this process never saw because it
// was ended in the seconds after launch - rare, and the safe direction for a
// number whose job is to say "this happens".
//
// OFF MEANS OFF. A disarmed ledger counts nothing, keeps nothing and reports
// zero; arming needs a real install id, which an opted-out run does not have;
// and the file is removed when reporting is switched off. The id inside the
// file means a copy left behind by an earlier identity is ignored rather than
// attributed to a new one.
//
// Thread safety: note() and settle() run on the watchdog and sender threads;
// arm(), disarm() and count() on the GUI thread. count() is one atomic read.
// The GUI thread takes a lock only to swap a pointer - never one held across
// file I/O.
class StallLedger {
public:
    // Far past anything a day of constant stalls produces (the watchdog files
    // one per stall and a stall takes at least five seconds); the Worker
    // clamps to the same number.
    static constexpr std::uint64_t kMaxCount = 100000;
    static constexpr char kFileName[] = "telemetry-stalls";

    StallLedger() = default;
    StallLedger(const StallLedger&) = delete;
    StallLedger& operator=(const StallLedger&) = delete;

    // Starts counting for `installId`, keeping the number in the file `path`
    // (UTF-8; empty keeps it in memory only, which is what a run with no
    // config directory gets). `loadExisting` reads a previous session's number
    // back - start-up only, never from the frame loop. An id that is not a
    // real install id disarms instead: with no identity there is nobody to
    // count for.
    void arm(const std::string& path, const std::string& installId, bool loadExisting);

    // Stops, forgets the number and removes the file. The removal runs on a
    // thread of its own so the Settings switch never waits on a disk.
    void disarm();

    bool armed() const { return armed_.load(std::memory_order_acquire); }

    // One more display stall. Called from the watchdog's thread. No-op when
    // disarmed.
    void note();

    // Stalls recorded and not yet accepted by the server. Zero when disarmed.
    std::uint64_t count() const;

    // A record that carried `carried` stalls was accepted: forget that many.
    void settle(std::uint64_t carried);

    // "<install id> <count>\n" - exposed for the test, which has to read it.
    static std::string fileText(const std::string& installId, std::uint64_t count);
    // The number in `text` when it belongs to `installId`, else false.
    static bool parseFileText(const std::string& text, const std::string& installId,
                              std::uint64_t& count);
    // The ledger's file inside the directory config.json lives in.
    static std::string pathIn(const std::string& configDir);
    // Removes `path`, ignoring every error.
    static void removeFile(const std::string& path);

private:
    struct Target {
        std::string path;
        std::string installId;
    };
    void persist();

    std::atomic<bool> armed_{false};
    std::atomic<std::uint64_t> count_{0};
    std::mutex targetMutex_;  // guards target_ ONLY; never held across I/O
    std::shared_ptr<const Target> target_;
    std::mutex ioMutex_;      // serialises the two threads that write the file
};

// What a send does with the ledger when it finishes: subtract `carried` if,
// and only if, the record was accepted.
TelemetryReporter::SendDone settleOnAccept(std::shared_ptr<StallLedger> ledger,
                                           std::uint64_t carried);

// The same for both ledgers: each is reduced by what the record carried, and
// only if the server accepted it.
TelemetryReporter::SendDone settleOnAccept(std::shared_ptr<StallLedger> stalls,
                                           std::uint64_t carriedStalls,
                                           std::shared_ptr<health::HealthLedger> healthLedger,
                                           health::Counts carriedHealth);

// `recordJson` with its `stalls` member set to `stalls`. The stored session
// record is journalled while the session runs, so its own number is only as
// fresh as the last save; the number actually sent is the ledger's at the
// moment of sending. A record that is not a JSON object is returned unchanged.
std::string withStalls(const std::string& recordJson, std::uint64_t stalls);

// START-UP'S DECISION about the previous session's pending record, in one place
// a test can reach. True when `outJson` is to be sent: the record with the
// ledger's count as of THIS moment (not the figure the last save journalled -
// the ledger holds a stall that no save ever saw), and `outCarried` the number
// to subtract if the server accepts it (pass both to settleOnAccept). False
// when nothing is to be sent: no pending record, or this exact stored record
// was already claimed by a launch (see claimReportSend, which this calls on the
// record AS STORED so the count changing cannot make a duplicate). An empty
// `configDir` has nowhere to claim and fails open, as claimReportSend does.
bool prepareStartupRecord(const std::string& configDir, const std::string& pendingJson,
                          const StallLedger& ledger, std::string& outJson,
                          std::uint64_t& outCarried);

// THE SAME DECISION WITH THE HEALTH COUNTS ALONGSIDE THE STALLS (0.99.64). The
// record sent also has its `health` member set to what the health ledger held for
// EARLIER sessions when this run started (HealthLedger::priorCounts) - never what
// this run has counted since, which describes a different session and goes with
// the next record. `outCarriedHealth` is what to take off if the server accepts
// it (pass both carried values to the four-argument settleOnAccept below).
bool prepareStartupRecord(const std::string& configDir, const std::string& pendingJson,
                          const StallLedger& stalls, const health::HealthLedger& healthLedger,
                          std::string& outJson, std::uint64_t& outCarriedStalls,
                          health::Counts& outCarriedHealth);

// How this copy was installed, read from the running program - nothing is
// written at install time. Windows: "store" when running as a Microsoft Store
// package (package_identity.hpp), otherwise "installer" (the only other
// Windows download is the installer). Linux: "appimage" when the AppImage
// runtime set APPIMAGE, otherwise "tarball". Android builds: "android".
// foxsdr.com and GitHub serve the SAME files, so they cannot be told apart.
std::string installChannel();

// Today's date in UTC as "YYYY-MM-DD" - a day, never a time, so the
// first-run field cannot single anybody out by the minute they installed.
std::string utcDateToday();

// The shapes the two first-run fields must have to be kept or sent. A
// hand-edited config that put anything else there is discarded on load,
// the same rule the install id follows.
bool validFirstRunDate(const std::string& s);
bool validFirstVersion(const std::string& s);

// SENDS ONE REPORT ONCE, across every process and every launch.
//
// The pending report lives in config.json and is only replaced when a later
// save writes the new session's report - so every launch that dies before
// that first save, and every copy started while another is still coming up
// (there is no single-instance guard), used to send the SAME report again.
// Measured 2026-09-28: one install's 9 h 22 m report arrived fifteen times in
// one second, and 86 duplicate rows in a month made total running time on the
// usage dashboard read 19% high.
//
// Clearing the report after sending cannot fix the second case: copies that
// start together all read it before any of them writes. So the right to send
// is CLAIMED by creating a marker file named after the report's content, with
// an exclusive create that exactly one process can win. The winner deletes the
// markers of older reports; a marker that already exists means this exact
// report was already handed to a sender.
//
// `dir` is the UTF-8 directory config.json lives in. Returns false only when
// the marker for this report already exists. Any other failure - no directory,
// a read-only disk - returns true: the old behaviour, a possible duplicate,
// is better than silently never reporting again.
bool claimReportSend(const std::string& dir, const std::string& json);

// The marker's file name for a report: "telemetry-sent-" plus a 64-bit FNV-1a
// of the payload in hex. Exposed for the test, which has to find it.
std::string reportSendMarkerName(const std::string& json);

// "Still running" heartbeats - the one question the startup report cannot
// answer. A report arrives when the app STARTS, describing the session that
// already ended, so the dataset knows who launched and never who still has
// the application open. A minimal beat every few minutes while it runs makes
// "instances running right now" a measurement instead of a model.
//
// The payload is the smallest thing that can be counted - the install id,
// the app version, and a beat marker - and NOTHING from the session: no
// modes, no panels, no radio, no durations. See PRIVACY.md.
//
// Beats land in their OWN dataset on the Worker (foxsdr_heartbeat), never in
// foxsdr_usage: every existing reader treats one usage row as one launch, and
// a beat every five minutes would multiply launches, stability and daily
// actives by ~12 per running hour.
class HeartbeatSender {
public:
    HeartbeatSender() = default;
    ~HeartbeatSender();  // joins the in-flight beat, same rule as the reporter

    HeartbeatSender(const HeartbeatSender&) = delete;
    HeartbeatSender& operator=(const HeartbeatSender&) = delete;

    // Arms (or re-arms) the sender. Refuses an empty url and an id that is
    // not exactly newInstallId()'s shape, so an opt-out run - which clears
    // the id - can call this and be certain nothing will ever beat.
    void configure(const std::string& url, const std::string& installId,
                   const std::string& appVersion, std::uint64_t intervalSec = 300);

    // Call often (once per frame is fine) with a monotonic seconds clock.
    // Fires a beat when one is due and never blocks: a beat still in flight
    // when the next is due - a network that black-holes for minutes - SKIPS
    // that beat rather than queueing threads behind a dead socket. The first
    // beat goes on the first poll after configure(), so a session shorter
    // than the interval still counts as running.
    void poll(double now);

    // True when poll(now) would fire. The schedule, testable without a
    // network or a thread.
    bool due(double now) const;

    // Exactly what goes on the wire, exposed so the payload test can hold
    // the privacy promise to it.
    static std::string beatJson(const std::string& id, const std::string& v);

private:
    std::string url_;
    std::string id_;
    std::string v_;
    std::uint64_t interval_ = 300;
    bool configured_ = false;
    bool firstSent_ = false;
    double nextAt_ = 0.0;
    std::thread thread_;
    std::atomic<bool> done_{false};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TELEMETRY_HPP
