// problem_report.hpp - the client half of the built-in "REPORT A BUG /
// DISLIKE" key, the second key in the STATUS column, directly under REQUEST A
// FEATURE: what gets chosen and typed, what gets validated, what gets sent.
//
// THE CONTRACT, agreed with the receiving end (site problems.go, 2026-09-23;
// the optional `diagnostics` field 2026-09-28):
//
//   POST https://foxsdr.com/api/problem-report   Content-Type: application/json
//   { schema:1, kind, text, contact, version, platform, arch, diagnostics? }
//   kind is "bug" or "dislike" and nothing else
//   diagnostics, when present, is the same text "Copy diagnostics" copies,
//     scrubbed and capped at kProblemReportDiagnosticsMaxBytes bytes; ABSENT
//     (never an empty string) when the person left the box unticked
//   200 {"ok":true}
//   400 {"ok":false,"error":"<sentence>"}   405 | 413 | 415
//   429 {"ok":false,"error":"<sentence>"}, with a Retry-After header
//
// SEVEN FIELDS ALWAYS, AN EIGHTH SOMETIMES: the feature request's six
// (feature_request.hpp) plus the kind the person chose, exactly as before -
// and now one more, `diagnostics`, present only when the "Attach the
// diagnostics log" box on the page was left ticked when SEND was pressed. No
// install id, no hardware serial, no config, no frequency, no location, no
// crash report - and no plugin list or anything else beyond what the
// diagnostics bundle (core/diag_report.hpp) already carries when the person
// chooses to attach it. Sent ONLY when the person presses SEND.
//
// WHY A SEPARATE OPTIONAL FIELD RATHER THAN ALWAYS SENDING IT EMPTY. Users
// believed a report already carried their log - it never did - and the fix
// the owner asked for is to let them attach it, truthfully, not to start
// sending something by default that the page's own sentence has always said
// it does not. `diagnostics` therefore behaves unlike every field above it:
// present means attached, and there is no other state to be empty in (the
// bundle text is never empty - buildDiagnosticsBundle() always writes at
// least its header).
//
// AN OLDER SITE (OR THIS ONE, THE MOMENT BEFORE ITS OWN CHANGE LANDS) DOES
// NOT KNOW THIS FIELD. problems.go decodes with DisallowUnknownFields, so an
// eighth key it does not expect fails the WHOLE decode with HTTP 400 and the
// generic "that did not arrive as valid JSON" - indistinguishable, on
// purpose, from genuinely malformed JSON. ProblemReportSendFlow below is
// exactly this fallback: a report that fails with 400 while it carried
// `diagnostics` is retried once, immediately, with the field left out, and
// diagnosticsDropped() tells the page to say so plainly. Every OTHER 400 (a
// real validation sentence - the text too short, an unknown kind) fails the
// retry the same way it failed the first attempt, so the person still sees
// the true reason.
//
// WHY THIS FILE IS SMALL. Everything a problem report shares with a feature
// request is REUSED, not copied:
//   - the bounds (10..100000 characters of text, 120 of contact, the contact
//     buffer size) and the functions that count and judge them, because the server
//     applies the feature request's bounds to this endpoint by reference and
//     the two pages must agree with it and with each other;
//   - platform/arch vocabulary (featureRequestPlatform()/featureRequestArch());
//   - the whole sender state machine - one bounded, cancellable worker, the
//     30 s cooldown, the Retry-After honouring - through
//     FeatureRequestSender::sendJson(), which takes a ready-built body. That
//     class has a test file of its own (tests/test_feature_request.cpp)
//     covering hang, cancel, refusal, 400 and 429; a second copy of it here
//     would be a second thing to get wrong in exactly the ways it already
//     does not.
// What is genuinely new lives here: the payload with its kind, the JSON
// builder, the field inventory PRIVACY.md is held to, the kind's own
// validation, and the endpoint.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PROBLEM_REPORT_HPP
#define CASCADE_CORE_PROBLEM_REPORT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/feature_request.hpp"

namespace cascade::core {

// The whole vocabulary of `kind`. A bug is something that does not work; a
// dislike is something that works as designed and that the person would
// rather it did not. The site triages the two apart.
inline constexpr const char* kProblemKindBug = "bug";
inline constexpr const char* kProblemKindDislike = "dislike";

// The diagnostics attachment's own ceiling, in bytes AFTER scrubbing - the
// contract's number, agreed with the site so a request this client is
// willing to send is never one the server's own body-size limit refuses
// first. truncateDiagnosticsForReport() is what keeps a bundle under it.
constexpr std::size_t kProblemReportDiagnosticsMaxBytes = 65536;

struct ProblemReportPayload {
    std::string kind;      // kProblemKindBug | kProblemKindDislike
    std::string text;      // what the person typed - TRIMMED by the builder
    std::string contact;   // optional email or callsign - TRIMMED likewise
    std::string version;   // cascade::versionString()
    std::string platform;  // featureRequestPlatform()
    std::string arch;      // featureRequestArch()
    // The scrubbed, size-capped diagnostics bundle text, or EMPTY when the
    // person left "Attach the diagnostics log" unticked. Empty is the only
    // sentinel for "not attached": a real bundle is never empty (see the
    // file header). problemReportJson() omits the field entirely when this
    // is empty, rather than sending an empty string.
    std::string diagnostics;
};

// True for exactly "bug" and "dislike" - case-sensitive, because the server
// is.
bool isProblemReportKind(const std::string& kind);

// Builds the exact seven-field JSON body. `text` and `contact` are trimmed
// here, the same way and for the same reason featureRequestJson() trims them:
// the length the server counts is the length after trimming.
std::string problemReportJson(const ProblemReportPayload& p);

// THE FIELD INVENTORY. PRIVACY.md documents these seven fields, and
// tests/test_problem_report.cpp compares this list with what
// problemReportJson() actually emits and with PRIVACY.md's own table.
const std::vector<std::string>& problemReportFieldNames();

// THE ONE OPTIONAL FIELD BEYOND THE SEVEN ABOVE - {"diagnostics"}, present
// only when a report is sent with it attached. Kept separate from
// problemReportFieldNames() rather than folded into it because every field
// in THAT list is always in the JSON, even when empty (`contact`), and
// `diagnostics` is the one field that is either present with real content or
// not there at all. PRIVACY.md's table carries this field too, and
// tests/test_problem_report.cpp checks both: the seven alone when a payload
// carries no diagnostics, the seven plus this one when it does.
const std::vector<std::string>& problemReportOptionalFieldNames();

// Empty when the kind is one of the two; otherwise the sentence the page
// shows beside its disabled SEND key. The page starts with NEITHER chosen, so
// every report says which it is because the person said so, not because a
// default was left standing.
std::string validateProblemReportKind(const std::string& kind);

// Text and contact are judged by the feature request's own functions
// (validateFeatureRequestText/Contact, featureRequestTextCharCount and
// friends): same bounds, same counting, same sentences. These two exist so
// the page and the tests name what they are checking.
inline std::string validateProblemReportText(const std::string& text) {
    return validateFeatureRequestText(text);
}
inline std::string validateProblemReportContact(const std::string& contact) {
    return validateFeatureRequestContact(contact);
}
// As above: the same minimal "might this be an email address" signal the
// feature-request page's warning uses, named for this page's own tests.
inline bool problemReportContactHasEmailAddress(const std::string& contact) {
    return featureRequestContactHasEmailAddress(contact);
}

// ---------------------------------------------------------------------------
// The "Attach the diagnostics log" checkbox
// ---------------------------------------------------------------------------

// The checkbox's own default, applied whenever the person (re)chooses a kind
// - true for a bug (the log is usually the fastest way to see what actually
// happened), false for a dislike (a preference report rarely has a fault in
// it worth attaching, and most are typed without one). The person can still
// tick or untick either one afterwards; this is only what the box shows the
// moment the kind changes, never a rule the SEND path itself enforces.
bool problemReportDefaultAttachDiagnostics(const std::string& kind);

// A raw diagnostics bundle (core::buildDiagnosticsBundle - the same bytes
// "Copy diagnostics" produces), made safe to leave the machine a second time.
// The bundle is already scrubbed of paths and log lines
// (core::scrubUploadPath/scrubUploadLog, applied inside buildDiagnosticsBundle
// itself); this is a belt-and-braces pass over the WHOLE text with
// core::maskAccountNames, plus the profile-root environment variable's own
// value (%USERPROFILE% on Windows, $HOME elsewhere) for a redirected profile
// that has no "\Users\" or "/home/" segment for maskAccountNames to find.
// Idempotent: scrubbing already-scrubbed text changes nothing further. Adds
// nothing the bundle does not already contain - no install id, nothing this
// function invents.
std::string scrubDiagnosticsForReport(const std::string& bundleText);

// Keeps `text` at or under `maxBytes`: the header (everything up to and
// including the "--- log ---\n" marker buildDiagnosticsBundle() writes)
// stays whole, and as many of the NEWEST log lines as fit are kept, dropping
// the oldest first (DiagLog::ringSnapshot() already orders them oldest
// first), with one line saying "(earlier lines dropped)" where the cut was
// made. Text already at or under the limit, or carrying no recognisable
// "--- log ---" marker, is returned unchanged.
std::string truncateDiagnosticsForReport(
    const std::string& text, std::size_t maxBytes = kProblemReportDiagnosticsMaxBytes);

// THE ONE FUNCTION THE PAGE ACTUALLY CALLS: scrub then truncate, in that
// order (the byte cap applies to what is actually sent, i.e. the scrubbed
// text), so the "Show what will be sent" preview and the bytes that go into
// ProblemReportPayload::diagnostics are built by calling this once and using
// the result for both - never two passes that could disagree.
std::string prepareDiagnosticsForReport(const std::string& rawBundleText);

// WHETHER A CACHED prepareDiagnosticsForReport() RESULT IS STALE. Building
// the attachment is real work - a ring snapshot under a mutex, the plugin
// list, a whole-bundle scrub pass - and drawProblemReportPage() would
// otherwise repeat it every single frame the "Attach the diagnostics log"
// checkbox is left ticked, unlike Copy diagnostics, which only ever runs
// once per press. True exactly when there is no cache yet, the log has
// genuinely grown since it was made (`currentLines != cachedLines` - DiagLog
// ::linesWritten(), which counts every line ever written, not just the ones
// still in the ring), or a whole second has passed (`currentEpoch !=
// cachedEpoch` - both whole-second clocks, so this is naturally "at most
// once a second" without a separate timer). A named, tested rule rather than
// inline arithmetic in the draw function (2026-09-28 review, N2).
inline bool problemReportDiagCacheStale(bool cacheValid, std::uint64_t cachedLines,
                                        std::uint64_t currentLines, std::uint64_t cachedEpoch,
                                        std::uint64_t currentEpoch) {
    return !cacheValid || currentLines != cachedLines || currentEpoch != cachedEpoch;
}

// ---------------------------------------------------------------------------
// The older-site fallback
// ---------------------------------------------------------------------------

// True when a report that carried `diagnostics` failed with exactly the
// failure an older site (or this one, before its own `diagnostics` change
// lands) gives an unknown JSON field - HTTP 400 - and a retry without the
// field is worth trying. False whenever `sentDiagnostics` is false (nothing
// to drop) or the status was anything else (a real validation sentence, a
// 429, a network failure): none of those are fixed by leaving the log out,
// and retrying them would only spend the sender's cooldown for nothing.
bool problemReportShouldRetryWithoutDiagnostics(bool sentDiagnostics, int httpStatus);

// WHERE REPORTS GO. https://foxsdr.com/api/problem-report, overridable by the
// env var FOXSDR_PROBLEM_URL - read fresh on every call through both the
// Windows and the POSIX environment, exactly as featureRequestEndpoint() reads
// FOXSDR_FEATURE_URL and for the same reason (a statically linked test binary
// and the code under test can see different copies of the environment).
std::string problemReportEndpoint();

// The sender: FeatureRequestSender, driven through sendJson() with
// problemReportJson(). A name of its own so AppWindow's member says what it
// carries.
using ProblemReportSender = FeatureRequestSender;

// ---------------------------------------------------------------------------
// ProblemReportSendFlow - a ProblemReportSender with the older-site fallback
// built in
// ---------------------------------------------------------------------------
//
// AppWindow drives exactly one of these instead of a bare ProblemReportSender
// so the fallback is not a second thing the GUI has to get right on top of
// everything FeatureRequestSender already does. It behaves exactly like
// ProblemReportSender in every state that class already defines - the same
// Idle/Sending/Sent/Failed/CoolingDown, the same cooldown, the same
// Retry-After honouring - with one addition: a send() whose payload carried
// `diagnostics` and which then fails with HTTP 400 is retried ONCE,
// immediately (through a second, short-lived sender, because the first one's
// own 30 s cooldown has already started and must not be bypassed for the
// person's NEXT report), with the field left out. Whichever send is now the
// live one - the original, or the retry - is what state()/lastStatus()/
// failureMessage()/blockedUntil() report; diagnosticsDropped() says whether
// the retry ever happened, for the sentence the page shows either way.
class ProblemReportSendFlow {
public:
    ProblemReportSendFlow() = default;
    ~ProblemReportSendFlow() = default;
    ProblemReportSendFlow(const ProblemReportSendFlow&) = delete;
    ProblemReportSendFlow& operator=(const ProblemReportSendFlow&) = delete;

    // Same contract as ProblemReportSender::sendJson(): false and starts
    // nothing when already sending or still cooling down. Builds both the
    // full body and, kept ready in case the fallback is needed, the body
    // with `diagnostics` left out.
    bool send(const std::string& url, const ProblemReportPayload& payload,
              std::uint64_t nowEpoch);

    // Called once a frame, exactly like ProblemReportSender::poll(). Performs
    // the one retry itself, the frame the first attempt's 400 is first seen.
    void poll(std::uint64_t nowEpoch);

    void cancel();

    FeatureRequestState state() const;
    std::string failureMessage() const;
    std::uint64_t blockedUntil() const;
    int lastStatus() const;
    bool busy() const;

    // True from the moment the fallback retries a send onward, whatever that
    // retry's own outcome - cleared again by the next send(). The page reads
    // this once the state is terminal to decide whether to say the log could
    // not be sent, regardless of whether the report itself ended up Sent or
    // Failed.
    bool diagnosticsDropped() const { return dropped_; }

private:
    ProblemReportSender primary_;
    std::unique_ptr<ProblemReportSender> retry_;
    std::string endpoint_;
    std::string retryBody_;  // the payload's JSON with `diagnostics` left out
    bool hadDiagnostics_ = false;
    bool retried_ = false;
    bool dropped_ = false;
    // The state primary_ was in as of the last poll(), so the Sending ->
    // Failed edge that triggers the fallback is seen exactly once.
    FeatureRequestState lastPrimaryState_ = FeatureRequestState::Idle;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PROBLEM_REPORT_HPP
