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

        std::set<std::string> actual;
        for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
        const std::set<std::string> declared(problemReportFieldNames().begin(),
                                             problemReportFieldNames().end());
        CHECK(declared.size() == 7);
        CHECK(actual == declared);

        // ...and PRIVACY.md's table names exactly the same seven.
        const std::set<std::string> documented = documentedProblemReportFields();
        CHECK(!documented.empty());
        CHECK(documented == declared);

        // THE RELATIONSHIP THE PRIVACY TEXT STATES: a feature request's six
        // fields plus `kind`, nothing else. A field added to one payload and
        // not the other fails here rather than making "the same as a feature
        // request plus the kind" quietly untrue.
        std::set<std::string> featurePlusKind(featureRequestFieldNames().begin(),
                                              featureRequestFieldNames().end());
        featurePlusKind.insert("kind");
        CHECK(declared == featurePlusKind);
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

    return testSummary("test_problem_report");
}
