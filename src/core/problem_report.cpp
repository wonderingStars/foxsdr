// See problem_report.hpp for the contract and for what is reused from
// feature_request.hpp rather than repeated here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/problem_report.hpp"

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

// Case-insensitive substring search, ASCII only - a profile directory name
// never needs non-ASCII case folding for this purpose, and this is the same
// convention diag_log.cpp's own lowerAscii() applies for the identical rule.
std::size_t findCaseInsensitive(const std::string& hay, const std::string& needle,
                                std::size_t from) {
    if (needle.empty() || hay.size() < needle.size() || from > hay.size() - needle.size()) {
        return std::string::npos;
    }
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a - 'A' + 'a'); }
            if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b - 'A' + 'a'); }
            if (a != b) { match = false; break; }
        }
        if (match) { return i; }
    }
    return std::string::npos;
}

// Every occurrence of the profile root's OWN VALUE, case-insensitive, becomes
// <user> - for a redirected profile (say D:\Profiles\steve) that has no
// "\Users\" or "/home/" segment anywhere in it for maskAccountNames to find.
// %USERPROFILE% on Windows, $HOME elsewhere - the same two variables a person
// would recognise their own account by, and different from scrubUploadPath's
// %LOCALAPPDATA%/$XDG_STATE_HOME/$HOME (which name where THIS application's
// files live, not necessarily the profile root itself).
void maskProfileRootValue(std::string& text) {
    std::string root;
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("USERPROFILE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) { return; }
    root.assign(buf, n);
#else
    const char* v = std::getenv("HOME");
    if (v == nullptr || v[0] == '\0') { return; }
    root = v;
#endif
    while (root.size() > 1 && (root.back() == '\\' || root.back() == '/')) { root.pop_back(); }
    if (root.size() < 2) { return; }
    static constexpr char kUser[] = "<user>";
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = findCaseInsensitive(text, root, from);
        if (at == std::string::npos) { break; }
        text.replace(at, root.size(), kUser);
        from = at + (sizeof(kUser) - 1);
    }
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
    // Present only when attached - see the struct's own comment. Not
    // trimmed: this is already the finished, scrubbed, size-capped bundle
    // text (prepareDiagnosticsForReport()), not a sentence the person typed.
    if (!p.diagnostics.empty()) { j["diagnostics"] = p.diagnostics; }
    // replace, not throw - a person's own words must never make this
    // unserialisable (see feature_request.cpp's dumpPayload()).
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

const std::vector<std::string>& problemReportFieldNames() {
    static const std::vector<std::string> names = {"schema",  "kind",     "text", "contact",
                                                    "version", "platform", "arch"};
    return names;
}

const std::vector<std::string>& problemReportOptionalFieldNames() {
    static const std::vector<std::string> names = {"diagnostics"};
    return names;
}

std::string validateProblemReportKind(const std::string& kind) {
    if (isProblemReportKind(kind)) { return std::string(); }
    // Marked for the catalogue; the report page translates it where it is drawn.
    return FOX_TR_NOOP("Please choose whether something is broken or something you dislike.");
}

bool problemReportDefaultAttachDiagnostics(const std::string& kind) {
    return kind == kProblemKindBug;
}

std::string scrubDiagnosticsForReport(const std::string& bundleText) {
    std::string out = bundleText;
    maskProfileRootValue(out);
    out = maskAccountNames(out);
    return out;
}

std::string truncateDiagnosticsForReport(const std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) { return text; }

    static const std::string kMarker = "\n--- log ---\n";
    const std::size_t markerAt = text.find(kMarker);
    if (markerAt == std::string::npos) {
        // No recognisable log section to trim from the middle of: nothing
        // safe to drop, so the cap simply cuts the end - at a whole
        // character, never part way through the UTF-8 bytes of one (a
        // plugin name, an SDR model, anything outside plain ASCII).
        return text.substr(0, utf8Floor(text.data(), maxBytes));
    }
    const std::size_t logStart = markerAt + kMarker.size();
    const std::string header = text.substr(0, logStart);
    const std::string logBody = text.substr(logStart);

    static const std::string kNotice = "(earlier lines dropped)\n";
    if (header.size() + kNotice.size() >= maxBytes) {
        // Unreachable at the real 64 KiB ceiling against a header of a few
        // hundred bytes; kept so a test exercising a small maxBytes cannot
        // produce an oversized result.
        return header.size() <= maxBytes ? header : header.substr(0, maxBytes);
    }
    const std::size_t budget = maxBytes - header.size() - kNotice.size();

    // Lines, each keeping the '\n' that ends it so every size summed below is
    // exactly the bytes that will be re-emitted. DiagLog::ringSnapshot() (and
    // so buildDiagnosticsBundle()) orders the log oldest first, newest last,
    // so the newest lines are the ones at the END of this list.
    std::vector<std::string> lines;
    {
        std::size_t pos = 0;
        while (pos < logBody.size()) {
            const std::size_t nl = logBody.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(logBody.substr(pos));
                break;
            }
            lines.push_back(logBody.substr(pos, nl - pos + 1));
            pos = nl + 1;
        }
    }

    std::size_t kept = 0;
    std::size_t used = 0;
    for (std::size_t i = lines.size(); i > 0; --i) {
        const std::string& line = lines[i - 1];
        if (used + line.size() > budget) { break; }
        used += line.size();
        ++kept;
    }

    std::string out = header;
    out += kNotice;
    for (std::size_t i = lines.size() - kept; i < lines.size(); ++i) { out += lines[i]; }
    return out;
}

std::string prepareDiagnosticsForReport(const std::string& rawBundleText) {
    // Scrub first, cap second: the byte ceiling applies to what is actually
    // sent, which is the scrubbed text.
    return truncateDiagnosticsForReport(scrubDiagnosticsForReport(rawBundleText));
}

bool problemReportShouldRetryWithoutDiagnostics(bool sentDiagnostics, int httpStatus) {
    return sentDiagnostics && httpStatus == 400;
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

// ---------------------------------------------------------------------------
// ProblemReportSendFlow
// ---------------------------------------------------------------------------

bool ProblemReportSendFlow::send(const std::string& url, const ProblemReportPayload& payload,
                                 std::uint64_t nowEpoch) {
    hadDiagnostics_ = !payload.diagnostics.empty();
    retried_ = false;
    dropped_ = false;
    retry_.reset();
    endpoint_ = url;
    ProblemReportPayload withoutDiag = payload;
    withoutDiag.diagnostics.clear();
    retryBody_ = problemReportJson(withoutDiag);
    const bool started = primary_.sendJson(url, problemReportJson(payload), nowEpoch);
    lastPrimaryState_ = primary_.state();
    return started;
}

void ProblemReportSendFlow::poll(std::uint64_t nowEpoch) {
    primary_.poll(nowEpoch);
    if (retry_) { retry_->poll(nowEpoch); }

    const FeatureRequestState now = primary_.state();
    if (lastPrimaryState_ == FeatureRequestState::Sending &&
        now == FeatureRequestState::Failed && !retried_ &&
        problemReportShouldRetryWithoutDiagnostics(hadDiagnostics_, primary_.lastStatus())) {
        retried_ = true;
        dropped_ = true;
        retry_ = std::make_unique<ProblemReportSender>();
        retry_->sendJson(endpoint_, retryBody_, nowEpoch);
    }
    lastPrimaryState_ = now;
}

void ProblemReportSendFlow::cancel() {
    primary_.cancel();
    if (retry_) { retry_->cancel(); }
}

FeatureRequestState ProblemReportSendFlow::state() const {
    return retry_ ? retry_->state() : primary_.state();
}

std::string ProblemReportSendFlow::failureMessage() const {
    return retry_ ? retry_->failureMessage() : primary_.failureMessage();
}

std::uint64_t ProblemReportSendFlow::blockedUntil() const {
    const std::uint64_t p = primary_.blockedUntil();
    return retry_ ? std::max(p, retry_->blockedUntil()) : p;
}

int ProblemReportSendFlow::lastStatus() const {
    return retry_ ? retry_->lastStatus() : primary_.lastStatus();
}

bool ProblemReportSendFlow::busy() const {
    return primary_.busy() || (retry_ && retry_->busy());
}

}  // namespace cascade::core
