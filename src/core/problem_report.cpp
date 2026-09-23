// See problem_report.hpp for the contract and for what is reused from
// feature_request.hpp rather than repeated here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/problem_report.hpp"

#include <cstdlib>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cascade::core {

namespace {

// The same trim featureRequestJson() applies (feature_request.cpp's
// trimFeatureWhitespace, which is file-local there): ASCII space, tab, CR and
// LF at either end. Repeated rather than exported because it is four lines,
// and test_problem_report.cpp pins the result against featureRequestJson()'s
// for the same input so the two cannot drift apart unnoticed.
std::string trimProblemWhitespace(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (a < b && isSpace(s[a])) { ++a; }
    while (b > a && isSpace(s[b - 1])) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

bool isProblemReportKind(const std::string& kind) {
    return kind == kProblemKindBug || kind == kProblemKindDislike;
}

std::string problemReportJson(const ProblemReportPayload& p) {
    nlohmann::json j;
    j["schema"] = 1;
    j["kind"] = p.kind;
    j["text"] = trimProblemWhitespace(p.text);
    j["contact"] = trimProblemWhitespace(p.contact);
    j["version"] = p.version;
    j["platform"] = p.platform;
    j["arch"] = p.arch;
    // replace, not throw - a person's own words must never make this
    // unserialisable (see feature_request.cpp's dumpPayload()).
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

const std::vector<std::string>& problemReportFieldNames() {
    static const std::vector<std::string> names = {"schema",  "kind",     "text", "contact",
                                                    "version", "platform", "arch"};
    return names;
}

std::string validateProblemReportKind(const std::string& kind) {
    if (isProblemReportKind(kind)) { return std::string(); }
    return "Please choose whether something is broken or something you dislike.";
}

std::string problemReportEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_PROBLEM_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* v = std::getenv("FOXSDR_PROBLEM_URL");
    if (v != nullptr && v[0] != '\0') { return std::string(v); }
#endif
    return "https://foxsdr.com/api/problem-report";
}

}  // namespace cascade::core
