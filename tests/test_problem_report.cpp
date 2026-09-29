// Tests for core/problem_report.hpp - the client half of the built-in
// "REPORT A BUG / DISLIKE" key.
//
// EVERY SEND IN THIS FILE GOES TO A LOCAL STUB ON 127.0.0.1 (http_post_stub.hpp),
// never to foxsdr.com - the endpoint block below proves the override this file
// relies on works before anything here sends, the same discipline
// tests/test_feature_request.cpp keeps.
//
// WHAT IS NOT RE-TESTED HERE: the sender's state machine. A problem report is
// sent through FeatureRequestSender::sendJson(), and every rule of that class
// (in-flight refusal, cooldown, Retry-After, the transport's own bound, cancel
// on destruction, a refused connection) is covered by test_feature_request.cpp,
// whose send() now runs through the same sendJson(). What IS tested here is
// everything the second body adds: the seventh field, its validation, the
// field inventory against PRIVACY.md, the endpoint and its override, and that
// the body the server receives on the problem route is byte for byte the one
// problemReportJson() built.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/feature_request.hpp"
#include "core/problem_report.hpp"
#include "test_check.hpp"

#include "http_post_stub.hpp"

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

template <typename T>
T at(const std::vector<T>& v, std::size_t i) {
    return (i < v.size()) ? v[i] : T();
}

nlohmann::json parseOrEmpty(const std::string& s) {
    nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return nlohmann::json::object(); }
    return j;
}

ProblemReportPayload samplePayload(const std::string& kind = "bug",
                                   const std::string& text =
                                       "The waterfall freezes when I change the sample rate",
                                   const std::string& contact = "") {
    ProblemReportPayload p;
    p.kind = kind;
    p.text = text;
    p.contact = contact;
    p.version = "0.99.20-test";
    p.platform = "windows";
    p.arch = "x64";
    return p;
}

FeatureRequestState waitForTerminal(ProblemReportSender& sender, std::uint64_t nowEpoch,
                                    int timeoutMs = 8000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        sender.poll(nowEpoch);
        const FeatureRequestState st = sender.state();
        if (st != FeatureRequestState::Sending) { return st; }
        if (std::chrono::steady_clock::now() >= deadline) { return st; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Same as above, for the fallback-aware flow: poll() is what performs the
// one retry, so this must call it exactly as the GUI's per-frame poll does,
// never the bare sender's.
FeatureRequestState waitForFlowTerminal(cascade::core::ProblemReportSendFlow& flow,
                                        std::uint64_t nowEpoch, int timeoutMs = 8000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        flow.poll(nowEpoch);
        const FeatureRequestState st = flow.state();
        if (st != FeatureRequestState::Sending) { return st; }
        if (std::chrono::steady_clock::now() >= deadline) { return st; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value == nullptr) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value, 1);
    }
#endif
}

// ---------------------------------------------------------------------------
// PRIVACY.md, parsed - the table under "What is sent when you report a bug or
// a dislike", the same flat-table reader test_feature_request.cpp uses for its
// own section.
// ---------------------------------------------------------------------------
std::vector<std::string> backtickedIdents(const std::string& cell) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t a = cell.find('`', pos);
        if (a == std::string::npos) { break; }
        const std::size_t b = cell.find('`', a + 1);
        if (b == std::string::npos) { break; }
        const std::string tok = cell.substr(a + 1, b - a - 1);
        bool ident = !tok.empty();
        for (char c : tok) {
            if (!std::isalnum(static_cast<unsigned char>(c))) { ident = false; }
        }
        if (ident) { out.push_back(tok); }
        pos = b + 1;
    }
    return out;
}

std::set<std::string> documentedProblemReportFields() {
    std::set<std::string> fields;
    std::ifstream in(fs::path(CASCADE_SOURCE_DIR) / "PRIVACY.md", std::ios::binary);
    const std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t start = doc.find("## What is sent when you report a bug or a dislike");
    if (start == std::string::npos) { return fields; }
    const std::size_t end = doc.find("\n## ", start + 4);
    const std::string section = doc.substr(start, (end == std::string::npos) ? end : end - start);

    std::size_t pos = 0;
    while (pos < section.size()) {
        const std::size_t nl = section.find('\n', pos);
        const std::string line =
            section.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? section.size() : nl + 1;
        if (line.rfind("| `", 0) != 0) { continue; }
        const std::size_t bar = line.find('|', 1);
        if (bar == std::string::npos) { continue; }
        for (const std::string& n : backtickedIdents(line.substr(1, bar - 1))) { fields.insert(n); }
    }
    return fields;
}

}  // namespace

int main() {
    // --- THE PAYLOAD, and the field inventory in every direction ------------
    {
        const ProblemReportPayload p =
            samplePayload("dislike", "  \n the tuning knob is far too sensitive  \n", "  g4xyz  ");
        const nlohmann::json j = parseOrEmpty(problemReportJson(p));
        CHECK(j.value("schema", 0) == 1);
        CHECK(j.value("kind", std::string()) == "dislike");
        CHECK(j.value("text", std::string()) == "the tuning knob is far too sensitive");
        CHECK(j.value("contact", std::string()) == "g4xyz");
        CHECK(j.value("version", std::string()) == "0.99.20-test");
        CHECK(j.value("platform", std::string()) == "windows");
        CHECK(j.value("arch", std::string()) == "x64");
        // No diagnostics attached in this sample - the eighth field is
        // ABSENT, not an empty string.
        CHECK(!j.contains("diagnostics"));

        std::set<std::string> actual;
        for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
        const std::set<std::string> mandatory(problemReportFieldNames().begin(),
                                              problemReportFieldNames().end());
        CHECK(mandatory.size() == 7);
        CHECK(actual == mandatory);

        // The full inventory PRIVACY.md's table names - the seven mandatory
        // fields plus the one optional `diagnostics` - whether or not THIS
        // particular payload attached it.
        std::set<std::string> everyField = mandatory;
        for (const std::string& f : problemReportOptionalFieldNames()) { everyField.insert(f); }
        CHECK(everyField.size() == 8);
        const std::set<std::string> documented = documentedProblemReportFields();
        CHECK(!documented.empty());
        CHECK(documented == everyField);

        // THE RELATIONSHIP THE PRIVACY TEXT STATES: a feature request's six
        // fields plus `kind`, nothing else MANDATORY. A field added to one
        // payload and not the other fails here rather than making "the same
        // as a feature request plus the kind" quietly untrue.
        std::set<std::string> featurePlusKind(featureRequestFieldNames().begin(),
                                              featureRequestFieldNames().end());
        featurePlusKind.insert("kind");
        CHECK(mandatory == featurePlusKind);
    }

    // --- THE OPTIONAL EIGHTH FIELD: present only when attached --------------
    {
        ProblemReportPayload p = samplePayload("bug");
        CHECK(p.diagnostics.empty());
        const nlohmann::json j0 = parseOrEmpty(problemReportJson(p));
        CHECK(!j0.contains("diagnostics"));

        p.diagnostics =
            "FoxSDR diagnostics bundle\ngenerated: 2026-09-28 00:00:00\n--- log ---\nline one\n";
        const nlohmann::json j1 = parseOrEmpty(problemReportJson(p));
        CHECK(j1.contains("diagnostics"));
        CHECK(j1.value("diagnostics", std::string()) == p.diagnostics);
        std::set<std::string> actual;
        for (auto it = j1.begin(); it != j1.end(); ++it) { actual.insert(it.key()); }
        std::set<std::string> full(problemReportFieldNames().begin(), problemReportFieldNames().end());
        for (const std::string& f : problemReportOptionalFieldNames()) { full.insert(f); }
        CHECK(actual == full);
    }

    // --- THE CHECKBOX'S OWN DEFAULT: ticked for a bug, not for a dislike ----
    {
        CHECK(problemReportDefaultAttachDiagnostics(kProblemKindBug));
        CHECK(!problemReportDefaultAttachDiagnostics(kProblemKindDislike));
        // No kind chosen yet (the page opens this way) defaults to unticked,
        // like a dislike - nothing is attached until a kind says this is a
        // bug.
        CHECK(!problemReportDefaultAttachDiagnostics(""));
        CHECK(!problemReportDefaultAttachDiagnostics("other"));
    }

    // --- THE ATTACHMENT CACHE'S OWN RULE: rebuild on growth or once a
    //     second, never just because a frame went by (2026-09-28 review, N2) -
    {
        // No cache yet: always stale, whatever the clocks say.
        CHECK(problemReportDiagCacheStale(false, 0, 0, 0, 0));
        CHECK(problemReportDiagCacheStale(false, 5, 5, 100, 100));
        // A valid cache, nothing changed: NOT stale - this is the case that
        // matters, because it is the one a naive "rebuild every frame"
        // implementation gets wrong.
        CHECK(!problemReportDiagCacheStale(true, 5, 5, 100, 100));
        // The log grew, same second: stale.
        CHECK(problemReportDiagCacheStale(true, 5, 6, 100, 100));
        // A second passed, log unchanged: stale (the "at most once a second"
        // half of the rule, not the "only when the log grows" half).
        CHECK(problemReportDiagCacheStale(true, 5, 5, 100, 101));
        // Both changed: still just stale, not double-counted or anything odd.
        CHECK(problemReportDiagCacheStale(true, 5, 6, 100, 101));
    }

    // --- SCRUBBING THE ATTACHED LOG: every path in it loses the account
    //     name, structurally - never by knowing the name in advance ---------
    {
        // A Windows path with a realistic username, backslashes.
        {
            const std::string in =
                "log-path: C:\\Users\\johnsmith\\AppData\\Local\\FoxSDR\\logs\\foxsdr.log\n";
            const std::string out = scrubDiagnosticsForReport(in);
            CHECK(out.find("johnsmith") == std::string::npos);
            CHECK(out.find("<user>") != std::string::npos);
        }
        // The same, forward slashes.
        {
            const std::string in =
                "log-path: C:/Users/johnsmith/AppData/Local/FoxSDR/logs/foxsdr.log\n";
            const std::string out = scrubDiagnosticsForReport(in);
            CHECK(out.find("johnsmith") == std::string::npos);
            CHECK(out.find("<user>") != std::string::npos);
        }
        // Mixed case in the "Users" keyword itself.
        {
            const std::string in = "crash-dir: c:\\USERS\\JohnSmith\\AppData\\Local\\FoxSDR\\crashes\n";
            const std::string out = scrubDiagnosticsForReport(in);
            CHECK(out.find("JohnSmith") == std::string::npos);
            CHECK(out.find("<user>") != std::string::npos);
        }
        // A Linux /home path.
        {
            const std::string in = "log-path: /home/johnsmith/.local/state/foxsdr/logs/foxsdr.log\n";
            const std::string out = scrubDiagnosticsForReport(in);
            CHECK(out.find("johnsmith") == std::string::npos);
            CHECK(out.find("<user>") != std::string::npos);
        }
        // The SAME name appearing again elsewhere in a path segment that is
        // NOT a profile directory is left alone: this function recognises a
        // profile path structurally (the segment right after \Users\,
        // /users/ or /home/), never by knowing the person's name in advance
        // and stripping it wherever it appears.
        {
            const std::string in =
                "log-path: C:\\Users\\johnsmith\\AppData\\Local\\FoxSDR\\logs\\foxsdr.log\n"
                "10:00:00.000 info bookmark import: D:\\Data\\johnsmith\\dump.txt\n";
            const std::string out = scrubDiagnosticsForReport(in);
            CHECK(out.find("<user>") != std::string::npos);
            CHECK(out.find("D:\\Data\\johnsmith\\dump.txt") != std::string::npos);
        }
        // The profile root's OWN VALUE (%USERPROFILE% / $HOME), for a
        // redirected profile with no "\Users\" or "/home/" segment in it at
        // all for the structural rule above to find.
        {
#if defined(_WIN32)
            char saved[512] = {0};
            const DWORD savedLen = ::GetEnvironmentVariableA("USERPROFILE", saved, sizeof(saved));
            setEnv("USERPROFILE", "D:\\FoxProfiles\\johnsmith");
            const std::string in =
                "crash-dir: d:\\FOXPROFILES\\JohnSmith\\FoxSDR\\crashes\n";
            const std::string out = scrubDiagnosticsForReport(in);
            if (savedLen > 0 && savedLen < sizeof(saved)) {
                setEnv("USERPROFILE", std::string(saved, savedLen).c_str());
            }
#else
            const char* savedHome = std::getenv("HOME");
            const std::string savedHomeStr = savedHome != nullptr ? savedHome : std::string();
            setEnv("HOME", "/opt/foxprofiles/johnsmith");
            const std::string in = "crash-dir: /OPT/FOXPROFILES/JohnSmith/FoxSDR/crashes\n";
            const std::string out = scrubDiagnosticsForReport(in);
            if (savedHome != nullptr) {
                setEnv("HOME", savedHomeStr.c_str());
            } else {
                setEnv("HOME", nullptr);
            }
#endif
            CHECK(out.find("johnsmith") == std::string::npos);
            CHECK(out.find("JohnSmith") == std::string::npos);
            CHECK(out.find("<user>") != std::string::npos);
        }
        // Scrubbing already-scrubbed text is a no-op: idempotent.
        {
            const std::string in = "log-path: C:\\Users\\johnsmith\\logs\\foxsdr.log\n";
            const std::string once = scrubDiagnosticsForReport(in);
            const std::string twice = scrubDiagnosticsForReport(once);
            CHECK(once == twice);
        }
    }

    // --- TRUNCATION: the header and the NEWEST lines survive, oldest first
    //     to go, and the cut says so -----------------------------------------
    {
        const std::string header =
            "FoxSDR diagnostics bundle\ngenerated: 2026-09-28 00:00:00\nversion: 0.99.42\n"
            "log-lines-total: 3\n\n--- log ---\n";
        std::string logBody;
        for (int i = 0; i < 400; ++i) {
            logBody += "00:00:00.000 info OLDEST_LINE_" + std::to_string(i) +
                       " padding padding padding padding\n";
        }
        logBody += "00:00:01.000 info NEWEST_LINE_MARKER padding padding padding\n";
        const std::string full = header + logBody;

        // Comfortably under the cap: unchanged, byte for byte.
        CHECK(truncateDiagnosticsForReport(full, full.size() + 100) == full);

        // Forced small: the header and the newest line survive, the oldest do
        // not, the total stays under the cap, and the cut is announced.
        const std::size_t cap = header.size() + 300;
        const std::string cut = truncateDiagnosticsForReport(full, cap);
        CHECK(cut.size() <= cap);
        CHECK(cut.rfind(header, 0) == 0);
        CHECK(cut.find("(earlier lines dropped)") != std::string::npos);
        CHECK(cut.find("NEWEST_LINE_MARKER") != std::string::npos);
        CHECK(cut.find("OLDEST_LINE_0 ") == std::string::npos);

        // Text with no "--- log ---" marker at all: the cap simply cuts the
        // end rather than guessing at a shape that is not there.
        const std::string noMarker(500, 'x');
        const std::string cutNoMarker = truncateDiagnosticsForReport(noMarker, 100);
        CHECK(cutNoMarker.size() == 100);
        CHECK(cutNoMarker == noMarker.substr(0, 100));

        // ...and that cut lands on a whole UTF-8 character, never part way
        // through one's bytes (2026-09-28 review, N6). 97 ASCII bytes then a
        // four-byte emoji (U+1F4E1): a cap of 100 lands one byte short of the
        // emoji's fourth byte, splitting it, unless the cut is floored back
        // to where the character starts.
        {
            std::string withEmoji(97, 'x');
            withEmoji += "\xF0\x9F\x93\xA1";  // U+1F4E1, a satellite antenna
            withEmoji += std::string(50, 'y');
            const std::string cutEmoji = truncateDiagnosticsForReport(withEmoji, 100);
            CHECK(cutEmoji.size() == 97);  // the whole partial emoji dropped, not 3 of its 4 bytes
            CHECK(cutEmoji == withEmoji.substr(0, 97));
            // The byte that would have been kept alone is a continuation
            // byte (0x80-0xBF) - proof this is genuinely the failure mode
            // being guarded against, not a cap that happened to land clean.
            CHECK((static_cast<unsigned char>(withEmoji[99]) & 0xC0) == 0x80);
        }

        // prepareDiagnosticsForReport is scrub-then-cap in one call, and its
        // result is what the page both shows and sends - proved here by
        // comparing it against doing the two steps by hand.
        const std::string withPath = header + "log-path: C:\\Users\\johnsmith\\x\\y\n" + logBody;
        CHECK(prepareDiagnosticsForReport(withPath) ==
              truncateDiagnosticsForReport(scrubDiagnosticsForReport(withPath)));
    }

    // --- THE OLDER-SITE FALLBACK: a 400 caused by the unknown field is
    //     retried once, without it, and said so - a real 400 for anything
    //     else is not ---------------------------------------------------------
    {
        CHECK(problemReportShouldRetryWithoutDiagnostics(true, 400));
        CHECK(!problemReportShouldRetryWithoutDiagnostics(false, 400));
        CHECK(!problemReportShouldRetryWithoutDiagnostics(true, 429));
        CHECK(!problemReportShouldRetryWithoutDiagnostics(true, 200));
    }
    {
        // A server that behaves exactly like problems.go's
        // DisallowUnknownFields decoder against the CURRENT contract: it does
        // not know `diagnostics` yet, so a body carrying it fails the whole
        // decode - 400, "that did not arrive as valid JSON" - and anything
        // else is accepted.
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::RejectUnknownField));
        cascade::core::ProblemReportSendFlow flow;
        ProblemReportPayload p = samplePayload("bug");
        p.diagnostics = "FoxSDR diagnostics bundle\ngenerated: x\n--- log ---\nline\n";
        const std::uint64_t t0 = 4000;
        CHECK(flow.send(srv.url(), p, t0));
        CHECK(waitForFlowTerminal(flow, t0) == FeatureRequestState::Sent);
        CHECK(flow.lastStatus() == 200);
        CHECK(flow.diagnosticsDropped());
        // Exactly two requests reached the server: the one that carried
        // diagnostics (refused) and the retry that did not (accepted) -
        // never a third.
        CHECK(srv.connections() == 2);
        const std::vector<std::string> bodies = srv.bodies();
        CHECK(bodies.size() == 2);
        CHECK(at(bodies, 0).find("\"diagnostics\"") != std::string::npos);
        CHECK(at(bodies, 1).find("\"diagnostics\"") == std::string::npos);
        srv.stop();
    }
    {
        // THE SAME FALLBACK, but polled ONE STEP AT A TIME and checked after
        // every single step - the shape drawProblemReportPage() actually
        // polls in (poll() once, then read state()/lastStatus() for that
        // frame) - to prove the primary attempt's 400 is never the value an
        // observer sees once a retry is going to happen (2026-09-28 review,
        // N1). ProblemReportSendFlow::poll() creates and starts the retry
        // within the SAME call that observes the primary's failure
        // (FeatureRequestSender::sendJson sets its state to Sending
        // synchronously, before returning), so the state a caller reads
        // immediately after any given poll() is either still Sending or the
        // retry's own outcome - never "Failed, 400" for a request that
        // carried the log.
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::RejectUnknownField));
        cascade::core::ProblemReportSendFlow flow;
        ProblemReportPayload p = samplePayload("bug");
        p.diagnostics = "FoxSDR diagnostics bundle\ngenerated: x\n--- log ---\nline\n";
        const std::uint64_t t0 = 4100;
        CHECK(flow.send(srv.url(), p, t0));
        int falseFailureFrames = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
        FeatureRequestState st = FeatureRequestState::Sending;
        while (st == FeatureRequestState::Sending &&
              std::chrono::steady_clock::now() < deadline) {
            flow.poll(t0);
            st = flow.state();
            // The one combination that must never be observed: Failed at
            // 400 while this flow ever carried diagnostics and has not yet
            // (or ever) retried without it. Once retried_ is true the retry
            // OWNS state()/lastStatus(), so a genuine 400 from the RETRY
            // itself (a real validation failure) is not this bug and is
            // covered by the "retry also fails" test instead.
            if (st == FeatureRequestState::Failed && flow.lastStatus() == 400 &&
                !flow.diagnosticsDropped()) {
                ++falseFailureFrames;
            }
            if (st == FeatureRequestState::Sending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        CHECK(st == FeatureRequestState::Sent);
        CHECK(falseFailureFrames == 0);
        srv.stop();
    }
    {
        // A report with NOTHING attached that happens to fail with 400 (a
        // real validation sentence) is never retried: there is nothing the
        // fallback could drop, and problemReportShouldRetryWithoutDiagnostics
        // says so above.
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::BadRequest400));
        cascade::core::ProblemReportSendFlow flow;
        const ProblemReportPayload p = samplePayload("bug");
        CHECK(p.diagnostics.empty());
        const std::uint64_t t0 = 4500;
        CHECK(flow.send(srv.url(), p, t0));
        CHECK(waitForFlowTerminal(flow, t0) == FeatureRequestState::Failed);
        CHECK(!flow.diagnosticsDropped());
        CHECK(srv.connections() == 1);
        srv.stop();
    }
    {
        // A report WITH diagnostics attached against a server that refuses
        // EVERYTHING with a real validation 400: the first attempt's failure
        // still looks exactly like "the site does not know this field" (any
        // 400 while diagnostics was sent triggers the fallback), so the
        // retry fires - and then fails the SAME way, because the real
        // problem was never the log. diagnosticsDropped() is still true (the
        // retry genuinely happened), but the message the page would show is
        // the server's own real sentence, not a diagnostics-specific one -
        // the GUI's own gating on this (drawProblemReportPage: the "could not
        // be sent" note is shown only when the overall state is Sent) is what
        // stops the two from being said in the same breath (2026-09-28
        // review, N4).
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::BadRequest400));
        cascade::core::ProblemReportSendFlow flow;
        ProblemReportPayload p = samplePayload("bug");
        p.diagnostics = "FoxSDR diagnostics bundle\ngenerated: x\n--- log ---\nline\n";
        const std::uint64_t t0 = 4600;
        CHECK(flow.send(srv.url(), p, t0));
        CHECK(waitForFlowTerminal(flow, t0) == FeatureRequestState::Failed);
        CHECK(flow.diagnosticsDropped());
        CHECK(flow.lastStatus() == 400);
        CHECK(flow.failureMessage() == "text must be between 10 and 2000 characters");
        // Both attempts reached the server: the fallback really did retry,
        // it just could not rescue a report that was going to fail anyway.
        CHECK(srv.connections() == 2);
        srv.stop();
    }

    // --- THE TRIM AGREES WITH THE FEATURE REQUEST'S -------------------------
    //
    // problem_report.cpp repeats the four-line trim rather than exporting
    // feature_request.cpp's; this pins the two together on the awkward cases.
    {
        for (const std::string raw : {std::string("\r\n\t  words in the middle \t\r\n"),
                                      std::string("   "), std::string(""),
                                      std::string("\n\nkeep\ninner\nlines\n\n")}) {
            FeatureRequestPayload f;
            f.text = raw;
            f.contact = raw;
            ProblemReportPayload pr = samplePayload("bug", raw, raw);
            const nlohmann::json fj = parseOrEmpty(featureRequestJson(f));
            const nlohmann::json pj = parseOrEmpty(problemReportJson(pr));
            CHECK(fj.value("text", std::string("x")) == pj.value("text", std::string("y")));
            CHECK(fj.value("contact", std::string("x")) == pj.value("contact", std::string("y")));
        }
    }

    // --- THE KIND: exactly two values, and none by default -------------------
    {
        CHECK(isProblemReportKind("bug"));
        CHECK(isProblemReportKind("dislike"));
        CHECK(!isProblemReportKind(""));
        CHECK(!isProblemReportKind("Bug"));
        CHECK(!isProblemReportKind("bug "));
        CHECK(!isProblemReportKind("crash"));
        CHECK(!isProblemReportKind("feature"));
        CHECK(validateProblemReportKind("bug").empty());
        CHECK(validateProblemReportKind("dislike").empty());
        // An unchosen kind - what the page opens with - refuses in words.
        CHECK(!validateProblemReportKind("").empty());
        CHECK(!validateProblemReportKind("other").empty());
        // A default-constructed payload carries no kind: nothing picks one
        // for the person.
        CHECK(ProblemReportPayload{}.kind.empty());
    }

    // --- TEXT AND CONTACT BOUNDS: the feature request's, exactly -------------
    {
        CHECK(!validateProblemReportText("").empty());
        CHECK(!validateProblemReportText(std::string(30, ' ')).empty());
        CHECK(!validateProblemReportText(std::string(9, 'a')).empty());
        CHECK(validateProblemReportText(std::string(10, 'a')).empty());
        CHECK(validateProblemReportText(std::string(kFeatureRequestMaxChars, 'a')).empty());
        CHECK(!validateProblemReportText(std::string(kFeatureRequestMaxChars + 1, 'a')).empty());
        // Characters, not bytes: nine three-byte characters are nine.
        std::string nine;
        for (int i = 0; i < 9; ++i) { nine += "\xE4\xBD\xA0"; }
        CHECK(!validateProblemReportText(nine).empty());
        CHECK(validateProblemReportText(nine + "\xE4\xBD\xA0").empty());
        CHECK(validateProblemReportContact("").empty());
        CHECK(validateProblemReportContact(std::string(kFeatureRequestMaxContactChars, 'c')).empty());
        CHECK(!validateProblemReportContact(std::string(kFeatureRequestMaxContactChars + 1, 'c'))
                   .empty());
    }

    // --- FOXSDR_PROBLEM_URL is honoured, the default is the real host, and it
    //     is a different seam from FOXSDR_FEATURE_URL ------------------------
    {
        setEnv("FOXSDR_PROBLEM_URL", nullptr);
        setEnv("FOXSDR_FEATURE_URL", nullptr);
        CHECK(problemReportEndpoint() == "https://foxsdr.com/api/problem-report");
        // Pointing the FEATURE seam somewhere does not move this one: a dev
        // run that redirects only one of the two must not have the other
        // silently follow it (or silently not).
        setEnv("FOXSDR_FEATURE_URL", "http://127.0.0.1:9/api/feature-request");
        CHECK(problemReportEndpoint() == "https://foxsdr.com/api/problem-report");
        setEnv("FOXSDR_FEATURE_URL", nullptr);
        setEnv("FOXSDR_PROBLEM_URL", "http://127.0.0.1:9");
        CHECK(problemReportEndpoint() == "http://127.0.0.1:9");
        CHECK(problemReportEndpoint().rfind("http://127.0.0.1:", 0) == 0);
        CHECK(featureRequestEndpoint() == "https://foxsdr.com/api/feature-request");
        setEnv("FOXSDR_PROBLEM_URL", nullptr);
    }

    // --- A real POST to the problem route, and a real 200 accepted ----------
    {
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::Accept200));
        CHECK(srv.url().find("/api/problem-report") != std::string::npos);
        ProblemReportSender sender;
        const std::uint64_t t0 = 1000;
        const ProblemReportPayload p = samplePayload("bug");
        const std::string body = problemReportJson(p);
        CHECK(sender.sendJson(srv.url(), body, t0));
        CHECK(waitForTerminal(sender, t0) == FeatureRequestState::Sent);
        CHECK(sender.lastStatus() == 200);
        CHECK(srv.connections() == 1);
        // THE BODY THE SERVER RECEIVED IS BYTE FOR BYTE THE BUILDER'S JSON,
        // kind and all.
        CHECK(at(srv.bodies(), 0) == body);
        CHECK(parseOrEmpty(at(srv.bodies(), 0)).value("kind", std::string()) == "bug");
        const std::string hdr = at(srv.requests(), 0);
        CHECK(hdr.find("application/json") != std::string::npos);
#if defined(_WIN32)
        // The Windows stub keeps the raw request head: prove the route.
        CHECK(hdr.rfind("POST /api/problem-report ", 0) == 0);
#endif
        // The feature request's 30 s cooldown applies to this sender too.
        CHECK(!sender.sendJson(srv.url(), body, t0 + 1));
        CHECK(sender.blockedUntil() == t0 + kFeatureRequestCooldownSeconds);
        CHECK(srv.connections() == 1);
        srv.stop();
    }

    // --- A refusal's sentence reaches the page ------------------------------
    {
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::BadRequest400));
        ProblemReportSender sender;
        const std::uint64_t t0 = 2000;
        CHECK(sender.sendJson(srv.url(), problemReportJson(samplePayload("dislike")), t0));
        CHECK(waitForTerminal(sender, t0) == FeatureRequestState::Failed);
        CHECK(sender.lastStatus() == 400);
        CHECK(sender.failureMessage() == "text must be between 10 and 2000 characters");
        srv.stop();
    }

    // --- 429 with Retry-After: 120 -> CoolingDown for the whole 120 s --------
    {
        StubServer srv("/api/problem-report");
        CHECK(srv.start(StubServer::Mode::RateLimit429));
        ProblemReportSender sender;
        const std::uint64_t t0 = 3000;
        CHECK(sender.sendJson(srv.url(), problemReportJson(samplePayload()), t0));
        CHECK(waitForTerminal(sender, t0) == FeatureRequestState::CoolingDown);
        CHECK(sender.failureMessage() == "slow down, please");
        CHECK(sender.blockedUntil() == t0 + 120);
        srv.stop();
    }

    // --- THE TWO HALVES AGAINST EACH OTHER, only when asked ------------------
    //
    // With FOXSDR_PROBLEM_E2E_URL pointing at a REAL foxsdr-site binary on
    // loopback, this sends what the application sends and reads what the
    // server really answers: both kinds accepted, a kind the server does not
    // know refused with its own sentence, and the five-an-hour limit. It
    // refuses any host but 127.0.0.1, so it cannot be aimed at production.
    {
        const char* e2e = std::getenv("FOXSDR_PROBLEM_E2E_URL");
        if (e2e == nullptr || e2e[0] == '\0') {
            std::printf("SKIPPED: end-to-end against a real site binary "
                        "(set FOXSDR_PROBLEM_E2E_URL=http://127.0.0.1:<port>/api/problem-report)\n");
        } else {
            const std::string url = e2e;
            CHECK(url.rfind("http://127.0.0.1:", 0) == 0);
            if (url.rfind("http://127.0.0.1:", 0) == 0) {
                const std::uint64_t t0 = 5000;
                // A kind the server does not know: refused before the rate
                // limiter is charged, with the server's own sentence.
                {
                    ProblemReportSender sender;
                    CHECK(sender.sendJson(url, problemReportJson(samplePayload("crash")), t0));
                    CHECK(waitForTerminal(sender, t0) == FeatureRequestState::Failed);
                    CHECK(sender.lastStatus() == 400);
                    std::printf("e2e bad kind: HTTP %d, %s\n", sender.lastStatus(),
                                sender.failureMessage().c_str());
                    CHECK(sender.failureMessage().find("bug or dislike") != std::string::npos);
                }
                std::string cyr;
                for (int i = 0; i < 1500; ++i) { cyr += "\xD0\xB6"; }
                int accepted = 0;
                int limited = 0;
                for (int i = 0; i < 6; ++i) {
                    ProblemReportSender sender;
                    const char* kind = (i % 2 == 0) ? "bug" : "dislike";
                    CHECK(sender.sendJson(url, problemReportJson(samplePayload(kind, cyr, "g4xyz")), t0));
                    const FeatureRequestState st = waitForTerminal(sender, t0);
                    if (st == FeatureRequestState::Sent && sender.lastStatus() == 200) {
                        ++accepted;
                    } else if (sender.lastStatus() == 429) {
                        ++limited;
                        CHECK(!sender.failureMessage().empty());
                    } else {
                        std::printf("e2e send %d: state %d, HTTP %d, %s\n", i,
                                    static_cast<int>(st), sender.lastStatus(),
                                    sender.failureMessage().c_str());
                    }
                }
                std::printf("e2e: accepted %d, rate-limited %d\n", accepted, limited);
                CHECK(accepted == 5);
                CHECK(limited == 1);
            }
        }
    }

    // --- THE NO-EMAIL WARNING'S OWN SIGNAL, same rule as the feature-request
    //     page's, named for this page's tests -------------------------------
    {
        CHECK(!problemReportContactHasEmailAddress(""));
        CHECK(!problemReportContactHasEmailAddress("G4XYZ"));
        CHECK(problemReportContactHasEmailAddress("g4xyz@example.com"));
        CHECK(problemReportContactHasEmailAddress("  g4xyz@example.com  "));
    }

    // --- THE SDRPLAY DIAGNOSTIC RIDES IN THE SAME ATTACHMENT (0.99.50):
    //     scrubbed like the bundle, after its own marker, and the whole held
    //     to the 64 KB - the log's oldest lines go first --------------------
    {
        const std::string header =
            "FoxSDR diagnostics bundle\ngenerated: 2026-09-29 00:00:00\n\n--- log ---\n";
        std::string logBody;
        for (int i = 0; i < 2000; ++i) {
            logBody += "00:00:00.000 info OLD_LINE_" + std::to_string(i) +
                       " padding padding padding padding\n";
        }
        logBody += "00:00:01.000 info NEWEST_LOG_LINE\n";
        const std::string prepared = prepareDiagnosticsForReport(header + logBody);
        const std::string probe =
            "FoxSDR SDRplay diagnostic\nAPI library: C:\\Users\\carol\\x\\sdrplay_api.dll\n"
            "SUMMARY\n  1 open and start ...... PASS\n";

        // No probe: the attachment is exactly what it was.
        CHECK(appendProbeToDiagnosticsForReport(prepared, "") == prepared);

        const std::string both = appendProbeToDiagnosticsForReport(prepared, probe);
        CHECK(both.size() <= kProblemReportDiagnosticsMaxBytes);
        CHECK(both.find(kProbeAttachmentMarker) != std::string::npos);
        CHECK(both.find("  1 open and start ...... PASS") != std::string::npos);
        CHECK(both.find("carol") == std::string::npos);  // scrubbed like the bundle
        CHECK(both.rfind(header, 0) == 0);              // the bundle's header survives
        CHECK(both.find("NEWEST_LOG_LINE") != std::string::npos);
        CHECK(both.find(kProbeAttachmentMarker) > both.find("NEWEST_LOG_LINE"));

        // A tight budget: the log gives way, the probe does not.
        const std::size_t cap = header.size() + 400 + probe.size();
        const std::string tight = appendProbeToDiagnosticsForReport(prepared, probe, cap);
        CHECK(tight.size() <= cap);
        CHECK(tight.find("  1 open and start ...... PASS") != std::string::npos);
        CHECK(tight.find("(earlier lines dropped)") != std::string::npos);
        CHECK(tight.find("OLD_LINE_0 ") == std::string::npos);

        // A probe larger than the whole budget: its own tail is cut, and said.
        std::string huge = "FoxSDR SDRplay diagnostic\n";
        for (int i = 0; i < 3000; ++i) { huge += "  +   1.000 rate line padding padding padding\n"; }
        const std::string cutProbe = appendProbeToDiagnosticsForReport(prepared, huge);
        CHECK(cutProbe.size() <= kProblemReportDiagnosticsMaxBytes);
        CHECK(cutProbe.find("(the rest of the SDRplay diagnostic was cut to fit)") !=
              std::string::npos);
        CHECK(cutProbe.find("FoxSDR SDRplay diagnostic\n") != std::string::npos);
    }

    return testSummary("test_problem_report");
}
