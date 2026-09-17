// feature_request.hpp - the client half of the built-in "REQUEST A FEATURE"
// button: what gets typed, what gets validated, what gets sent, and how it
// reaches foxsdr.com without the GUI thread ever touching a socket.
//
// THE CONTRACT, agreed with the receiving end (2026-09-17):
//
//   POST https://foxsdr.com/api/feature-request   Content-Type: application/json
//   { schema:1, text, contact, version, platform, arch }
//   200 {"ok":true}
//   400 {"ok":false,"error":"<sentence>"}   405 | 413 | 415
//   429 {"ok":false,"error":"<sentence>"}, with a Retry-After header
//
// EXACTLY THOSE SIX FIELDS. No install id, no hardware, no log, no config, no
// frequency, no location, no plugin list - the request carries nothing this
// application knows about the machine or the session, only what the person
// just typed and which build they are running. Sent ONLY when the person
// presses SEND: never automatically, never retried in the background, never
// queued across a restart. Unlike core/crash_upload.hpp next to this file,
// there is no sweep and no persisted policy state, because there is nothing
// TO retry - a request that failed to send is still sitting in the text box
// exactly as the person typed it, where a crash report is a file on disk that
// has to be found again on a later start.
//
// THE TRANSPORT IS SHARED WITH THE CRASH UPLOADER, not reinvented.
// core::postBounded() (crash_upload.hpp) is the one bounded, cancellable
// HTTPS POST this application makes - WinHTTP on Windows, cpp-httplib and
// OpenSSL on Linux - and this file drives it with captureBody=true, which is
// the one behavioural difference from the crash path: this request's
// contract requires surfacing the server's own {"error":"..."} sentence on a
// 400 or a 429, which crash_upload.cpp's four rules deliberately do not do
// for a crash report. Writing a second HTTP client for that one field would
// have been the wrong trade against reusing the one that already has a
// loopback gate, certificate verification, no-redirect and a cancel token
// proven by tests/test_crash_upload.cpp.
//
// THE WORKER THREAD is the same shape as core::CrashUploader: send() starts a
// thread that runs the POST and publishes a small snapshot behind a mutex;
// poll() is called once a frame from the GUI thread and only ever reads that
// snapshot, so the window never blocks on the network. This application has
// shipped six hang fixes for exactly "a network or device call reached the
// window thread" (see the config-save-hang and radar-scope-removed commits),
// and this is not going to be the seventh. The destructor cancels then joins,
// exactly like CrashUploader::stop(): cancelling closes the live request
// handle, which is what makes a blocked call return in milliseconds instead
// of sitting out its receive timeout, so the join that follows is fast
// without needing a bounded abandon of its own - there is deliberately no
// second `constexpr std::chrono` wait here for tests/test_shutdown_budget.cpp
// to discover, because cancellation is what bounds the join, not a timer.
//
// WHAT NEVER LEAVES THIS PROCESS UNENCRYPTED OR UNASKED: the typed text and
// the typed contact line live in memory only, in AppWindow's own members
// (never in AppConfig, never in config.json, never in the diagnostics log -
// the log line this feature writes says how many characters were sent and
// what the server answered, never the words themselves).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FEATURE_REQUEST_HPP
#define CASCADE_CORE_FEATURE_REQUEST_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cascade::core {

// Forward-declared rather than pulling in the whole of crash_upload.hpp here:
// this header only ever holds a shared_ptr to one, and only feature_request.cpp
// needs the definition (to construct one and hand it to postBounded()).
class UploadCancel;

// ---------------------------------------------------------------------------
// The wire payload
// ---------------------------------------------------------------------------
struct FeatureRequestPayload {
    std::string text;      // what the person typed - TRIMMED by the builder
    std::string contact;   // optional email or callsign - TRIMMED likewise
    std::string version;   // cascade::versionString()
    std::string platform;  // "windows" | "linux" | "android"
    std::string arch;      // "x64" | "arm64"
};

// Builds the exact six-field JSON body the contract defines. `text` and
// `contact` are trimmed here (never assumed already trimmed by the caller),
// because the length the server counts is the length AFTER trimming and the
// bytes sent must be the bytes that length was measured against.
std::string featureRequestJson(const FeatureRequestPayload& p);

// THE FIELD INVENTORY, in the same spirit as crash_upload.hpp's
// uploadFieldNames(): PRIVACY.md documents these six fields, and
// tests/test_feature_request.cpp compares this list with what
// featureRequestJson() actually emits, and with PRIVACY.md's own table, in
// every direction - a field added on one side and not the other two fails
// loudly instead of drifting quietly.
const std::vector<std::string>& featureRequestFieldNames();

// The server's own bounds (feature-request-contract.md), mirrored here so the
// SEND key can be disabled before a doomed request is even built, and so a
// person sees the same sentence whether the client catches the problem or the
// server does.
constexpr std::size_t kFeatureRequestMinChars = 10;
constexpr std::size_t kFeatureRequestMaxChars = 2000;
constexpr std::size_t kFeatureRequestMaxContactChars = 120;

// UTF-8 CODE POINTS, NOT BYTES. A multi-byte letter must count once -
// counting bytes would let a message the server accepts be refused here, or
// refuse one here that the server would have accepted. Malformed UTF-8 never
// crashes this: a continuation byte with no lead byte in front of it simply
// is not counted as starting a new character, which undercounts a broken
// encoding rather than reading past the end of anything. This is the bare
// primitive with no knowledge of the contract's other rules (trimming,
// control characters) - featureRequestTextCharCount() and
// featureRequestContactCharCount() below are what validation and the GUI's
// on-screen counter actually use.
std::size_t featureRequestCharCount(const std::string& utf8Text);

// THE COUNT THE SERVER ACTUALLY JUDGES 10..2000 (text) and 0..120 (contact)
// AGAINST (feature-request-contract.md, updated 2026-09-17): after trimming,
// and after dropping control characters the same way the server does before
// it counts them - text keeps '\n' and '\t' (a message may have line
// breaks), contact keeps neither (it is meant to be one line). This is NOT
// what gets SENT: featureRequestJson() sends the trimmed text and contact
// verbatim, control characters and all, because stripping them for storage
// is the server's job, not this client's - these two exist purely so the
// number this client shows and enforces never disagrees with the number the
// server enforces.
std::size_t featureRequestTextCharCount(const std::string& text);
std::size_t featureRequestContactCharCount(const std::string& contact);

// Empty when valid; otherwise a sentence a person can read, describing the
// SAME bound the server enforces, judged against featureRequestTextCharCount()/
// featureRequestContactCharCount() above so the two never disagree about a
// boundary by accident.
std::string validateFeatureRequestText(const std::string& text);
std::string validateFeatureRequestContact(const std::string& contact);

// The contract's vocabulary for the running build - lowercase, and distinct
// from PluginRepo::hostOs()/hostArch() (plugin_repo.hpp), which speak a
// different vocabulary ("macos", "x86") for a different audience, the
// plugin catalogue. featureRequestArch() only ever answers "x64" or "arm64":
// this application ships no x86 build for either platform to name.
std::string featureRequestPlatform();
std::string featureRequestArch();

// WHERE REQUESTS GO. https://foxsdr.com/api/feature-request, overridable by
// the env var FOXSDR_FEATURE_URL - the same two seams as
// crashUploadEndpoint()/telemetryEndpoint()/updateEndpoint(): a Windows
// GetEnvironmentVariableA probe and a POSIX getenv() probe, read independently
// rather than through one cached value, because a statically linked TEST
// binary and the code under test can see different copies of the process
// environment on Windows (see the Lessons-learned entry this repeats from
// tests/test_dab and tests/test_drm) - reading it fresh on every call is what
// keeps a test's SetEnvironmentVariableA/setenv call visible to this function
// without a rebuild.
std::string featureRequestEndpoint();

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------
//
// Idle       - nothing in flight, SEND is available (text length permitting).
// Sending    - the worker thread owns a live request; SEND is disabled.
// Sent       - the last send was accepted (2xx); SEND stays disabled until
//              the post-send cooldown (kFeatureRequestCooldownSeconds) passes,
//              at which point poll() moves this back to Idle on its own.
// Failed     - the last send was refused, timed out, or could not reach the
//              server; failureMessage() names why. Same cooldown as Sent.
// CoolingDown- the server answered 429; failureMessage() carries its
//              sentence if it sent one, and blockedUntil() honours its
//              Retry-After (or the default cooldown if it sent none or an
//              unusable one). Same automatic return to Idle once it expires.
//
// A single lockout clock (blockedUntil()) governs send()'s refusal to start a
// second request in ALL three terminal states - Sent, Failed and CoolingDown
// alike - which is what makes "cooldown blocks a second send" one rule rather
// than three: the 30 s client-side cooldown the contract asks for after ANY
// send, and the server's own Retry-After after a 429, are the same field with
// two different sources for its value.
enum class FeatureRequestState { Idle, Sending, Sent, Failed, CoolingDown };

constexpr std::uint64_t kFeatureRequestCooldownSeconds = 30;
// A ceiling on a hostile or broken Retry-After, the same defensive clamp
// crash_upload.hpp's noteRateLimited() applies for the same header on the
// same server: it cannot mute this client for a year just because it sent a
// number that big.
constexpr std::uint64_t kFeatureRequestMaxBackoffSeconds = 24 * 60 * 60;

class FeatureRequestSender {
public:
    FeatureRequestSender() = default;
    // Cancels an in-flight send and joins - see the file header for why that
    // is fast rather than a bounded wait of its own.
    ~FeatureRequestSender();

    FeatureRequestSender(const FeatureRequestSender&) = delete;
    FeatureRequestSender& operator=(const FeatureRequestSender&) = delete;

    // Starts a send if idle (or a previous terminal state whose cooldown has
    // expired) and not already sending. Returns false and starts nothing
    // otherwise - the GUI is expected to have already disabled the SEND key
    // in exactly those states, so this is the belt-and-braces check for
    // anything that calls it another way, the same role decideUpload() plays
    // for the crash uploader. `nowEpoch` is the caller's clock, never read
    // from the system here, so a test can drive a cooldown to the second
    // without a real sleep.
    bool send(const std::string& url, const FeatureRequestPayload& payload,
              std::uint64_t nowEpoch);

    // Called once a frame. Joins a worker that has finished (the join is
    // immediate - the worker has already returned by the time it is seen to
    // be done) and, once a terminal state's cooldown has passed against
    // `nowEpoch`, moves the state back to Idle on its own so the SEND key
    // re-enables without anyone having to notice and clear it by hand.
    void poll(std::uint64_t nowEpoch);

    // Cancels an in-flight send and joins. Safe to call more than once and
    // when nothing is running, the same contract as CrashUploader::stop().
    void cancel();

    FeatureRequestState state() const;
    // Meaningful in Failed and CoolingDown; empty otherwise. The 429 case
    // carries the server's own sentence here when it sent one, so the same
    // accessor answers both "why did it fail" and "why am I waiting", which
    // is one field with two sources for it (see the enum's own comment).
    std::string failureMessage() const;
    // Epoch seconds before which send() refuses to start another request;
    // 0 when nothing is blocking it (Idle). Meaningful for the GUI's "(wait
    // Ns)" caption on the disabled SEND key.
    std::uint64_t blockedUntil() const;
    // The HTTP status the last completed send answered with, or 0 when it
    // never got one (a timeout, a refused connection, or nothing sent yet).
    // Exists for the log line the contract asks for ("feature request: sent,
    // N characters, HTTP 200") - failureMessage() is for a PERSON, this is
    // for the one line the application is allowed to write about a send that
    // never names what was typed.
    int lastStatus() const;
    bool busy() const;

private:
    struct Snapshot {
        FeatureRequestState state = FeatureRequestState::Idle;
        std::string failureMessage;
        std::uint64_t blockedUntil = 0;
        int status = 0;
    };

    mutable std::mutex mu_;
    Snapshot snap_;
    // Set by the worker as the very last thing it does, so a poll() that
    // observes it true is guaranteed the thread is about to return (POSIX
    // and Windows threads both permit joining a thread that has already
    // finished its function body - this is not a race, it is the ordinary
    // "the thread object outlives the function" case every std::thread has).
    std::atomic<bool> workerDone_{false};
    std::thread thread_;
    std::shared_ptr<UploadCancel> cancel_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_FEATURE_REQUEST_HPP
