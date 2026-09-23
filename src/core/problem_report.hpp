// problem_report.hpp - the client half of the built-in "REPORT A BUG /
// DISLIKE" key, the second key in the STATUS column, directly under REQUEST A
// FEATURE: what gets chosen and typed, what gets validated, what gets sent.
//
// THE CONTRACT, agreed with the receiving end (site problems.go, 2026-09-23):
//
//   POST https://foxsdr.com/api/problem-report   Content-Type: application/json
//   { schema:1, kind, text, contact, version, platform, arch }
//   kind is "bug" or "dislike" and nothing else
//   200 {"ok":true}
//   400 {"ok":false,"error":"<sentence>"}   405 | 413 | 415
//   429 {"ok":false,"error":"<sentence>"}, with a Retry-After header
//
// EXACTLY THOSE SEVEN FIELDS: the feature request's six (feature_request.hpp)
// plus the kind the person chose. No install id, no hardware, no log, no
// config, no frequency, no location, no plugin list, no crash report - a bug
// report typed into this box carries only what the person chose and typed
// and which build they are running. Sent ONLY when the person presses SEND.
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

#include <string>
#include <vector>

#include "core/feature_request.hpp"

namespace cascade::core {

// The whole vocabulary of `kind`. A bug is something that does not work; a
// dislike is something that works as designed and that the person would
// rather it did not. The site triages the two apart.
inline constexpr const char* kProblemKindBug = "bug";
inline constexpr const char* kProblemKindDislike = "dislike";

struct ProblemReportPayload {
    std::string kind;      // kProblemKindBug | kProblemKindDislike
    std::string text;      // what the person typed - TRIMMED by the builder
    std::string contact;   // optional email or callsign - TRIMMED likewise
    std::string version;   // cascade::versionString()
    std::string platform;  // featureRequestPlatform()
    std::string arch;      // featureRequestArch()
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

}  // namespace cascade::core

#endif  // CASCADE_CORE_PROBLEM_REPORT_HPP
