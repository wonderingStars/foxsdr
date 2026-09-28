// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tester_usage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

std::string rfc3339Utc(std::time_t t) {
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

namespace {

// The inverse of rfc3339Utc(), for finalizeTesterUsageReport()'s clamp below.
// Returns false (and leaves `out` untouched) for anything not in exactly
// that shape - a malformed value is left for the caller to replace outright
// rather than guessed at.
bool parseRfc3339Utc(const std::string& s, std::time_t& out) {
    std::tm tmv{};
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (s.size() != 20 ||
        std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &min,
                    &sec) != 6) {
        return false;
    }
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    tmv.tm_isdst = 0;
#if defined(_WIN32)
    const std::time_t t = ::_mkgmtime(&tmv);
#else
    const std::time_t t = ::timegm(&tmv);
#endif
    if (t == static_cast<std::time_t>(-1)) { return false; }
    out = t;
    return true;
}

}  // namespace

TesterUsageReport finalizeTesterUsageReport(TesterUsageReport r, std::time_t now) {
    if (r.version.size() > kTesterUsageMaxVersionChars) {
        r.version.resize(kTesterUsageMaxVersionChars);
    }
    if (r.token.size() > kTesterUsageMaxTokenChars) { r.token.resize(kTesterUsageMaxTokenChars); }

    // LOWER-CASED, THEN RE-DEDUPLICATED - two spellings of one kind must not
    // survive as two entries once case is no longer what tells them apart.
    {
        std::vector<std::string> lowered;
        lowered.reserve(r.session.radios.size());
        for (std::string kind : r.session.radios) {
            for (char& c : kind) {
                if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
            }
            if (!kind.empty() &&
                std::find(lowered.begin(), lowered.end(), kind) == lowered.end()) {
                lowered.push_back(std::move(kind));
            }
        }
        r.session.radios = std::move(lowered);
    }

    // SESSION START, CLAMPED TO THE SITE'S OWN WINDOW. A malformed stamp (it
    // should never be, since rfc3339Utc() is the only writer, but this file
    // does not trust its own callers any more than a hand-edited config) is
    // replaced with `now` outright rather than left to fail the site's own
    // validation with no chance to recover.
    std::time_t startT = now;
    if (!parseRfc3339Utc(r.session.start, startT)) {
        startT = now;
    } else {
        const std::int64_t ageSec = static_cast<std::int64_t>(now) - static_cast<std::int64_t>(startT);
        if (ageSec > kTesterUsageMaxSessionAgeSec) {
            startT = now - static_cast<std::time_t>(kTesterUsageMaxSessionAgeSec);
        } else if (ageSec < -kTesterUsageMaxSessionFutureSec) {
            startT = now + static_cast<std::time_t>(kTesterUsageMaxSessionFutureSec);
        }
    }
    r.session.start = rfc3339Utc(startT);

    // EACH PLUGIN'S MINUTES, CLAMPED TO THE SESSION'S OWN LENGTH - a plugin
    // cannot plausibly have run longer than the session that fed it, and
    // rounding each independently can otherwise put one a minute over by
    // coincidence rather than by anything meaningful having happened.
    for (TesterUsagePlugin& p : r.session.plugins) {
        if (p.minutes > r.session.minutes) { p.minutes = r.session.minutes; }
    }

    return r;
}

bool validTesterToken(const std::string& token) {
    if (token.size() != 32) { return false; }
    for (char c : token) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) { return false; }
    }
    return true;
}

std::string extractTesterToken(const std::string& input) {
    const std::string t = trim(input);
    if (t.empty()) { return std::string(); }

    // THE BARE TOKEN. Checked first and checked exactly - a 32-character
    // string that happens to look like a URL fragment (it cannot: the
    // alphabet is 0-9a-f only) would otherwise fall through to the parser
    // below for no reason.
    if (validTesterToken(t)) { return t; }

    // THE PRIVATE PORTAL LINK: https://foxsdr.com/#t=<token> (web/portal.js's
    // manageLink, "/#t=" + token). The token lives in the fragment, so this
    // is a plain string search rather than a URL parse - there is no server
    // this could leak to at this point, the string never left the clipboard.
    //
    // "#c=" (confirmLink, a single-use confirmation secret) is deliberately
    // NOT accepted here: it opens nothing this field is for, and beta.go
    // keeps the two in separate indexes precisely so one can never be
    // presented in place of the other.
    const std::size_t hash = t.find('#');
    if (hash == std::string::npos) { return std::string(); }
    std::string frag = t.substr(hash + 1);
    // The fragment may carry more than one key ("t=...&x=...", though the
    // site never emits that); split on '&' and look for "t=" among the
    // parts, the same way URLSearchParams would.
    std::size_t at = 0;
    while (at <= frag.size()) {
        const std::size_t amp = frag.find('&', at);
        const std::string part =
            frag.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (part.size() > 2 && part[0] == 't' && part[1] == '=') {
            const std::string candidate = trim(part.substr(2));
            if (validTesterToken(candidate)) { return candidate; }
        }
        if (amp == std::string::npos) { break; }
        at = amp + 1;
    }
    return std::string();
}

std::string maskTesterToken(const std::string& token) {
    if (token.size() <= 8) { return std::string(token.size(), '*'); }
    return token.substr(0, 4) + "..." + token.substr(token.size() - 4);
}

std::string testerUsagePlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#else
    return "linux";
#endif
}

namespace {
constexpr char kDefaultTesterUsageEndpoint[] = "https://foxsdr.com/api/tester-usage";
}  // namespace

std::string testerUsageEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* env = std::getenv("FOXSDR_TESTER_USAGE_URL");
    if (env != nullptr && env[0] != '\0') { return std::string(env); }
#endif
    return std::string(kDefaultTesterUsageEndpoint);
}

std::string TesterUsageReport::toJson() const {
    nlohmann::json j;
    j["token"] = token;
    j["version"] = version;
    j["platform"] = platform;
    j["arch"] = arch;
    nlohmann::json s;
    s["start"] = session.start;
    s["minutes"] = session.minutes;
    j["session"] = std::move(s);
    j["features"] = session.features;
    nlohmann::json plugins = nlohmann::json::array();
    for (const TesterUsagePlugin& p : session.plugins) {
        nlohmann::json e;
        e["id"] = p.id;
        e["version"] = p.version;
        e["minutes"] = p.minutes;
        plugins.push_back(std::move(e));
    }
    j["plugins"] = std::move(plugins);
    j["radios"] = session.radios;
    // replace, not throw: a plugin id/version or a hand-typed field is
    // third-party or user text, and a bad byte in one must cost one
    // replacement character rather than an exception out of a save/send path
    // (tests/test_json_dump_policy.cpp holds every dump() in this tree to
    // this rule).
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string maskedPreviewJson(const TesterUsageReport& report) {
    // BUILD FROM A COPY WITH THE TOKEN ALREADY MASKED, rather than
    // string-replacing the real token out of a finished document: a
    // string-replace approach breaks the moment the masked form and the
    // literal token happen to overlap another field's text, and it still
    // requires the real token to have been serialised at least once. This
    // way the full token is never written to a std::string that outlives
    // this call.
    TesterUsageReport masked = report;
    masked.token = maskTesterToken(report.token);
    return masked.toJson();
}

// ---------------------------------------------------------------------------
// Transport - a thin wrapper around core/crash_upload.hpp's shared, tested
// client (postBounded), not a second hand-rolled WinHTTP/httplib one. Both
// reporters need exactly the same thing: https except on loopback (so a test
// can use a plain socket without a certificate), abortable mid-flight, and
// nothing from the response but the status code.
// ---------------------------------------------------------------------------

TesterUsageOutcome postTesterUsage(const std::string& url, const std::string& json,
                                    const std::shared_ptr<UploadCancel>& cancel) {
    if (url.empty() || json.empty() || !cancel) { return TesterUsageOutcome::Retry; }
    try {
        const RawPostResult r = postBounded(url, json, cancel, /*captureBody=*/false);
        if (!r.attempted || r.cancelled) { return TesterUsageOutcome::Retry; }
        if (r.status >= 200 && r.status < 300) { return TesterUsageOutcome::Sent; }
        if (r.status == 401) { return TesterUsageOutcome::Invalid; }
        if (r.status == 400 || r.status == 413) { return TesterUsageOutcome::Rejected; }
        // 429 and anything else this contract has no name for: keep it,
        // never treat an unrecognised code as permission to discard a
        // report the site may yet accept.
        return TesterUsageOutcome::Retry;
    } catch (...) {
        // Silent by design, same rule as telemetry's transport: a usage
        // report that interrupted anything to complain would be worse than
        // one that quietly retries next launch.
        return TesterUsageOutcome::Retry;
    }
}

// ---------------------------------------------------------------------------
// The sender - detached, never joined (see the header for why that is safe)
// ---------------------------------------------------------------------------

TesterUsageSender::~TesterUsageSender() {
    // Best-effort only: closes the transport's handle so an in-flight worker
    // unblocks promptly instead of sitting out its own timeout with nobody
    // left to hear about it. Nothing here waits for that to happen - see the
    // header's note on why detaching is safe without it.
    if (cancel_) { cancel_->cancel(); }
}

bool TesterUsageSender::busy() const { return inFlight_ && inFlight_->load(); }

void TesterUsageSender::send(const std::string& url, const std::string& json) {
    if (url.empty() || json.empty()) { return; }
    if (busy()) { return; }
    cancel_ = std::make_shared<UploadCancel>();
    inFlight_ = std::make_shared<std::atomic<bool>>(true);
    outcomeSlot_ = std::make_shared<std::atomic<int>>(-1);
    auto cancel = cancel_;
    auto inFlight = inFlight_;
    auto slot = outcomeSlot_;
    // DETACHED, DELIBERATELY. Every capture below is a shared_ptr the thread
    // owns a reference to, never `this` and never a reference to anything
    // AppWindow owns - see the header's "SAFE TO DETACH" note for why that
    // is what makes never joining safe rather than merely convenient.
    std::thread([url, json, cancel, inFlight, slot]() {
        TesterUsageOutcome outcome = TesterUsageOutcome::Retry;
        try {
            outcome = postTesterUsage(url, json, cancel);
        } catch (...) {
            // Silent by design; outcome stays Retry.
        }
        // Skip publishing a result once cancellation has started: the owner
        // is on its way out, and the only thing left to do is not touch
        // anything it might already have released. slot/inFlight are our
        // own heap allocations either way, so writing them is never unsafe
        // by itself - this is about not reporting a stale-by-then outcome
        // nobody will read, not about memory safety.
        if (!cancel->cancelled()) { slot->store(static_cast<int>(outcome)); }
        inFlight->store(false);
    }).detach();
}

std::optional<TesterUsageOutcome> TesterUsageSender::takeOutcome() {
    if (!outcomeSlot_) { return std::nullopt; }
    const int v = outcomeSlot_->exchange(-1);
    if (v < 0) { return std::nullopt; }
    return static_cast<TesterUsageOutcome>(v);
}

// ---------------------------------------------------------------------------
// The bounded queue
// ---------------------------------------------------------------------------

void TesterUsageQueue::push(const std::string& reportJson) {
    if (reportJson.empty()) { return; }
    items_.push_back(reportJson);
    while (items_.size() > kMax) { items_.erase(items_.begin()); }
}

void TesterUsageQueue::removeFront() {
    if (!items_.empty()) { items_.erase(items_.begin()); }
}

void TesterUsageQueue::setItems(std::vector<std::string> items) {
    if (items.size() > kMax) {
        items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(items.size() - kMax));
    }
    items_ = std::move(items);
}

std::string tokenOfReport(const std::string& reportJson) {
    const nlohmann::json j = nlohmann::json::parse(reportJson, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return std::string(); }
    const auto it = j.find("token");
    if (it == j.end() || !it->is_string()) { return std::string(); }
    return it->get<std::string>();
}

void TesterUsageQueue::dropOthers(const std::string& token) {
    std::vector<std::string> kept;
    kept.reserve(items_.size());
    for (const std::string& item : items_) {
        if (tokenOfReport(item) == token) { kept.push_back(item); }
    }
    items_ = std::move(kept);
}

TesterUsageOutcomeEffect applyTesterUsageOutcome(TesterUsageOutcome outcome,
                                                  TesterUsageQueue& queue) {
    TesterUsageOutcomeEffect e;
    switch (outcome) {
        case TesterUsageOutcome::Sent:
        case TesterUsageOutcome::Rejected:
            // Sent: the site has it. Rejected (400/413): it never will
            // succeed by being retried - the contract's own words for this
            // response are to log it and drop it, exactly like a Sent one.
            queue.removeFront();
            break;
        case TesterUsageOutcome::Invalid:
            e.nowInvalid = true;
            e.stop = true;
            break;
        case TesterUsageOutcome::Retry:
            // 429 or a transport failure: leave it queued and stop trying
            // for the rest of THIS session - a black-holed endpoint must not
            // become a poll-rate retry loop.
            e.stop = true;
            break;
    }
    return e;
}

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------

void TesterUsageRecorder::noteFeature(const std::string& id) {
    if (!armed() || id.empty()) { return; }
    std::lock_guard<std::mutex> lk(mu_);
    if (features_.size() >= kMaxFeatures) { return; }
    if (std::find(features_.begin(), features_.end(), id) == features_.end()) {
        features_.push_back(id);
    }
}

void TesterUsageRecorder::noteRadio(const std::string& kind) {
    if (!armed() || kind.empty()) { return; }
    std::lock_guard<std::mutex> lk(mu_);
    if (radios_.size() >= kMaxRadios) { return; }
    if (std::find(radios_.begin(), radios_.end(), kind) == radios_.end()) {
        radios_.push_back(kind);
    }
}

void TesterUsageRecorder::accruePlugin(const std::string& id, const std::string& version,
                                        double seconds) {
    if (!armed() || id.empty() || !(seconds > 0.0)) { return; }
    std::lock_guard<std::mutex> lk(mu_);
    auto it = plugins_.find(id);
    if (it == plugins_.end()) {
        if (plugins_.size() >= kMaxPlugins) { return; }
        it = plugins_.emplace(id, Accrual{}).first;
    }
    it->second.version = version;  // the most recently seen version wins
    it->second.seconds += seconds;
}

std::vector<std::string> TesterUsageRecorder::features() const {
    std::lock_guard<std::mutex> lk(mu_);
    return features_;
}

std::vector<std::string> TesterUsageRecorder::radios() const {
    std::lock_guard<std::mutex> lk(mu_);
    return radios_;
}

std::vector<TesterUsagePlugin> TesterUsageRecorder::plugins() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TesterUsagePlugin> out;
    out.reserve(plugins_.size());
    for (const auto& [id, acc] : plugins_) {
        TesterUsagePlugin p;
        p.id = id;
        p.version = acc.version;
        // Rounded to nearest, not truncated: a plugin fed for 89 seconds
        // reports 1 minute, not 0 - see the header's note on accruePlugin.
        p.minutes = static_cast<std::uint64_t>(std::llround(acc.seconds / 60.0));
        out.push_back(std::move(p));
    }
    return out;
}

void TesterUsageRecorder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    features_.clear();
    radios_.clear();
    plugins_.clear();
}

// ---------------------------------------------------------------------------
// Catalogue id lookup
// ---------------------------------------------------------------------------

std::string catalogueIdForPlugin(const std::string& fileName,
                                  const std::vector<InstalledPlugin>& installed) {
    const std::string bare = fs::path(fileName).filename().string();
    for (const InstalledPlugin& ip : installed) {
        if (!bare.empty() && ip.file == bare) { return ip.id; }
    }
    // NO MANIFEST RECORD: reported under the fixed shared bucket rather than
    // a name made up from the display name - see kSideloadedPluginId.
    return kSideloadedPluginId;
}

}  // namespace cascade::core
