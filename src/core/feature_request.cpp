// See feature_request.hpp for the contract, why the transport is shared with
// the crash uploader, and why the worker thread is shaped the way it is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/feature_request.hpp"

#include "core/crash_upload.hpp"

#include <cstdlib>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cascade::core {

namespace {

// ASCII whitespace only, and DELIBERATELY including '\n' and '\r' - unlike
// crash_upload.cpp's own trimSpace(), which keeps '\n' because a log line's
// newline is part of the report. A feature request's text and contact are
// each ONE field: a text box's leading or trailing blank lines are what a
// person leaves behind clicking into the box and out of it again, never
// content they meant to send, and the character count the SEND key's
// "10 characters" reason is judged against has to agree with what
// featureRequestJson() actually sends - so both trim exactly this set.
std::string trimFeatureWhitespace(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (a < b && isSpace(s[a])) { ++a; }
    while (b > a && isSpace(s[b - 1])) { --b; }
    return s.substr(a, b - a);
}

// Drops control characters the way the server does before it counts or
// stores them (feature-request-contract.md, updated 2026-09-17): bytes below
// 0x20 and 0x7F (DEL), except that a caller asking to keep '\n'/'\t' gets to
// - text does, because a message is allowed line breaks; contact does not,
// because it is meant to be one line. Byte-wise on purpose: a control
// character is always a single ASCII byte in UTF-8 (the encoding of any code
// point above U+007F never produces a byte under 0x80), so this cannot
// mis-split a multi-byte character the way a careless byte-level filter
// could.
std::string stripControlCharsForCount(const std::string& s, bool keepNewlineAndTab) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool isControl = (c < 0x20u) || (c == 0x7Fu);
        if (isControl) {
            if (keepNewlineAndTab && (c == '\n' || c == '\t')) { out.push_back(static_cast<char>(c)); }
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// replace, not throw: third-party text (a person's own words) must never be
// able to make this unserialisable, the same discipline crash_upload.cpp's
// dumpPayload() and telemetry.cpp's TelemetryReport::toJson() both apply to
// the third-party strings they carry (a plugin name, here a sentence someone
// typed).
std::string dumpPayload(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

std::string featureRequestJson(const FeatureRequestPayload& p) {
    nlohmann::json j;
    j["schema"] = 1;
    j["text"] = trimFeatureWhitespace(p.text);
    j["contact"] = trimFeatureWhitespace(p.contact);
    j["version"] = p.version;
    j["platform"] = p.platform;
    j["arch"] = p.arch;
    return dumpPayload(j);
}

const std::vector<std::string>& featureRequestFieldNames() {
    static const std::vector<std::string> names = {"schema", "text", "contact",
                                                    "version", "platform", "arch"};
    return names;
}

std::size_t featureRequestCharCount(const std::string& utf8Text) {
    std::size_t n = 0;
    for (unsigned char c : utf8Text) {
        // A UTF-8 continuation byte (10xxxxxx) never starts a character; every
        // other byte value does - the ASCII range (0xxxxxxx) and every
        // multi-byte sequence's LEAD byte (11xxxxxx) alike. Counting lead
        // bytes rather than decoding full code points is enough to get the
        // COUNT right without needing a decoder that has to reject anything;
        // it also means malformed input degrades to an undercount rather than
        // to undefined behaviour.
        if ((c & 0xC0u) != 0x80u) { ++n; }
    }
    return n;
}

std::size_t featureRequestTextCharCount(const std::string& text) {
    return featureRequestCharCount(
        stripControlCharsForCount(trimFeatureWhitespace(text), /*keepNewlineAndTab=*/true));
}

std::size_t featureRequestContactCharCount(const std::string& contact) {
    return featureRequestCharCount(
        stripControlCharsForCount(trimFeatureWhitespace(contact), /*keepNewlineAndTab=*/false));
}

std::string validateFeatureRequestText(const std::string& text) {
    const std::size_t chars = featureRequestTextCharCount(text);
    if (chars < kFeatureRequestMinChars) {
        return "Please write at least " + std::to_string(kFeatureRequestMinChars) +
               " characters (" + std::to_string(chars) + " so far).";
    }
    if (chars > kFeatureRequestMaxChars) {
        return "Please keep it to " + std::to_string(kFeatureRequestMaxChars) +
               " characters or fewer (it is currently " + std::to_string(chars) + ").";
    }
    return std::string();
}

std::string validateFeatureRequestContact(const std::string& contact) {
    const std::size_t chars = featureRequestContactCharCount(contact);
    if (chars > kFeatureRequestMaxContactChars) {
        return "Please keep the contact line to " +
               std::to_string(kFeatureRequestMaxContactChars) +
               " characters or fewer (it is currently " + std::to_string(chars) + ").";
    }
    return std::string();
}

std::string featureRequestPlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    // Unreachable from this checkout (master builds Windows and Linux only -
    // see MEMORY.md's android-port entry), kept so the branch that DOES build
    // it needs no change here when it merges.
    return "android";
#else
    return "linux";
#endif
}

std::string featureRequestArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    // This application ships no x86 build of either platform to name, so
    // everything that is not ARM64 is x64 - matching the contract's own
    // two-value vocabulary rather than telemetry.cpp's archDescription(),
    // which also answers "x86" for an audience (the usage report) that wants
    // to know if one still exists in the wild.
    return "x64";
#endif
}

std::string featureRequestEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_FEATURE_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    // The same seam, read the POSIX way. getenv() returns nullptr when the
    // variable is unset; an empty value is treated as unset too, matching the
    // Windows probe's implicit behaviour for a zero-length result.
    const char* v = std::getenv("FOXSDR_FEATURE_URL");
    if (v != nullptr && v[0] != '\0') { return std::string(v); }
#endif
    return "https://foxsdr.com/api/feature-request";
}

// ---------------------------------------------------------------------------
// The response body's "error" sentence, when there is one
// ---------------------------------------------------------------------------
namespace {

std::string extractServerError(const std::string& body) {
    if (body.empty()) { return std::string(); }
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return std::string(); }
    return j.value("error", std::string());
}

}  // namespace

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------
FeatureRequestSender::~FeatureRequestSender() { cancel(); }

bool FeatureRequestSender::send(const std::string& url, const FeatureRequestPayload& payload,
                                std::uint64_t nowEpoch) {
    return sendJson(url, featureRequestJson(payload), nowEpoch);
}

bool FeatureRequestSender::sendJson(const std::string& url, const std::string& json,
                                    std::uint64_t nowEpoch) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (snap_.state == FeatureRequestState::Sending) { return false; }
        if (nowEpoch < snap_.blockedUntil) { return false; }
        snap_.state = FeatureRequestState::Sending;
        snap_.failureMessage.clear();
        snap_.blockedUntil = 0;
    }
    // A previous worker may be sitting here finished but uncollected - poll()
    // usually does this, but a caller that presses SEND the instant a
    // cooldown clears can beat the next poll() to it. The join is immediate:
    // workerDone_ is only ever set true after the worker's function body has
    // already returned.
    if (thread_.joinable()) { thread_.join(); }
    workerDone_.store(false, std::memory_order_relaxed);

    cancel_ = std::make_shared<UploadCancel>();
    std::shared_ptr<UploadCancel> cancel = cancel_;
    thread_ = std::thread([this, url, json, cancel, nowEpoch]() {
        const RawPostResult raw = postBounded(url, json, cancel, /*captureBody=*/true);

        FeatureRequestState st = FeatureRequestState::Failed;
        std::string msg;
        std::uint64_t blockedUntil = nowEpoch + kFeatureRequestCooldownSeconds;

        if (raw.cancelled) {
            // Cancelled means the destructor is tearing this object down (or
            // an explicit cancel() was called); nobody is watching state()
            // afterwards for a reason that matters, and there is no server
            // answer to hold anyone back from, so this leaves no lockout.
            st = FeatureRequestState::Idle;
            blockedUntil = 0;
        } else if (raw.status >= 200 && raw.status < 300) {
            st = FeatureRequestState::Sent;
        } else if (raw.status == 429) {
            st = FeatureRequestState::CoolingDown;
            msg = extractServerError(raw.body);
            std::uint64_t retry = (raw.rateLimited && raw.retryAfterSeconds > 0)
                                      ? raw.retryAfterSeconds
                                      : kFeatureRequestCooldownSeconds;
            // The same defensive clamp crash_upload.hpp's noteRateLimited()
            // applies to the identical header from the identical server: a
            // hostile or broken Retry-After cannot mute this client for a
            // year.
            if (retry > kFeatureRequestMaxBackoffSeconds) {
                retry = kFeatureRequestMaxBackoffSeconds;
            }
            blockedUntil = nowEpoch + retry;
        } else if (raw.attempted) {
            // A real answer, just not one this client accepts: 400, 413, 415,
            // 405, or anything else a future server version might send. The
            // server's own sentence is shown when it sent one - a 400 always
            // does, per the contract - and anything else falls back to a
            // plain sentence naming the status, so the status line is never
            // blank.
            msg = extractServerError(raw.body);
            if (msg.empty()) {
                msg = "The server would not accept the request (HTTP " +
                      std::to_string(raw.status) + ").";
            }
        } else {
            // No status at all: the connection was refused, the name did not
            // resolve, or the server accepted the connection and never
            // answered - the one case the contract names explicitly that an
            // HTTP status code cannot describe.
            msg = "Could not reach foxsdr.com. Check your connection and try again.";
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.state = st;
            snap_.failureMessage = msg;
            snap_.blockedUntil = blockedUntil;
            snap_.status = raw.status;
        }
        workerDone_.store(true, std::memory_order_release);
    });
    return true;
}

void FeatureRequestSender::poll(std::uint64_t nowEpoch) {
    if (workerDone_.load(std::memory_order_acquire) && thread_.joinable()) {
        thread_.join();
        workerDone_.store(false, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(mu_);
    // A terminal state whose lockout has expired returns to Idle on its own,
    // so the SEND key re-enables and the thank-you or the error sentence
    // clears without the GUI having to notice the clock and reset anything
    // by hand. Sending is never touched here - only the worker's own
    // completion moves it out of that state, above.
    if ((snap_.state == FeatureRequestState::Sent ||
         snap_.state == FeatureRequestState::Failed ||
         snap_.state == FeatureRequestState::CoolingDown) &&
        nowEpoch >= snap_.blockedUntil) {
        snap_.state = FeatureRequestState::Idle;
        snap_.failureMessage.clear();
        snap_.blockedUntil = 0;
    }
}

void FeatureRequestSender::cancel() {
    // Cancels first, then joins - the same order and the same reason as
    // CrashUploader::stop(): closing the live request handle is what makes a
    // transfer blocked on a server that never answers return immediately
    // instead of sitting out its receive timeout, so the join that follows
    // costs milliseconds rather than seconds.
    if (cancel_) { cancel_->cancel(); }
    if (thread_.joinable()) { thread_.join(); }
    workerDone_.store(false, std::memory_order_relaxed);
    cancel_.reset();
}

FeatureRequestState FeatureRequestSender::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_.state;
}

std::string FeatureRequestSender::failureMessage() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_.failureMessage;
}

std::uint64_t FeatureRequestSender::blockedUntil() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_.blockedUntil;
}

int FeatureRequestSender::lastStatus() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_.status;
}

bool FeatureRequestSender::busy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_.state == FeatureRequestState::Sending;
}

}  // namespace cascade::core
