// tester_link_net.cpp - the NETWORK half of core/tester_link.hpp: the site's address, the confirm-by-name lookup and the portal-token exchange, each on a worker. Split out of
// tester_link.cpp in stub campaign B15 with the code unchanged, so that the headless engine - which links the link-request file and the instance claim (the process boot's
// STEP 5, Host kind only) and nothing of this - does not link a call to the tester portal at all: cascade_engine is a static library, a binary takes only the objects it
// references, and nothing in src/engine, src/net or src/engine_main.cpp references this one. tests/test_tester_host_only.cpp holds that, by the sources and by the built
// image (the site's path is in cascade.exe and not in foxsdr-engine.exe).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tester_link.hpp"

#include "core/tester_usage.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {
constexpr char kDefaultBetaApiBaseUrl[] = "https://foxsdr.com";
}  // namespace

std::string betaApiBaseUrl() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_BETA_API_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* env = std::getenv("FOXSDR_BETA_API_URL");
    if (env != nullptr && env[0] != '\0') { return std::string(env); }
#endif
    return std::string(kDefaultBetaApiBaseUrl);
}

std::string sanitizeTesterName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7Fu) { continue; }
        out.push_back(c);
    }
    if (out.size() > kMaxTesterNameBytes) {
        std::size_t cut = kMaxTesterNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) { --cut; }
        out.resize(cut);
    }
    while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    return out;
}

BetaLinkResolved resolveAppTokenName(const std::string& baseUrl, const std::string& appToken,
                                     const std::shared_ptr<UploadCancel>& cancel) {
    BetaLinkResolved out;
    if (baseUrl.empty() || appToken.empty() || !cancel) { return out; }
    const AuthRequestResult r =
        authRequestBounded("GET", baseUrl + "/api/beta/app-token/me", appToken, "", cancel);
    if (!r.attempted || r.cancelled) { return out; }
    if (r.status == 404) {
        out.outcome = BetaLinkOutcome::Invalid;
        return out;
    }
    if (r.status < 200 || r.status >= 300) { return out; }  // NetworkError-shaped: leave as-is
    const nlohmann::json j = nlohmann::json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return out; }
    const auto it = j.find("name");
    if (it == j.end() || !it->is_string()) { return out; }
    out.outcome = BetaLinkOutcome::Ok;
    out.name = sanitizeTesterName(it->get<std::string>());
    return out;
}

struct BetaLinkNameSender::Shared {
    std::atomic<bool> inFlight{false};
    std::mutex mu;
    std::optional<BetaLinkResolved> result;
};

BetaLinkNameSender::~BetaLinkNameSender() {
    // Best-effort: unblocks an in-flight worker promptly. Never waits for it
    // - see TesterUsageSender's header comment for why detaching is safe.
    if (cancel_) { cancel_->cancel(); }
}

bool BetaLinkNameSender::busy() const { return shared_ && shared_->inFlight.load(); }

void BetaLinkNameSender::send(const std::string& baseUrl, const std::string& appToken) {
    if (baseUrl.empty() || appToken.empty() || busy()) { return; }
    cancel_ = std::make_shared<UploadCancel>();
    shared_ = std::make_shared<Shared>();
    shared_->inFlight.store(true);
    auto shared = shared_;
    auto cancel = cancel_;
    std::thread([baseUrl, appToken, shared, cancel]() {
        BetaLinkResolved r;
        try {
            r = resolveAppTokenName(baseUrl, appToken, cancel);
        } catch (...) {
            // r stays default-constructed (NetworkError)
        }
        if (!cancel->cancelled()) {
            std::lock_guard<std::mutex> lk(shared->mu);
            shared->result = r;
        }
        shared->inFlight.store(false);
    }).detach();
}

std::optional<BetaLinkResolved> BetaLinkNameSender::takeResult() {
    if (!shared_) { return std::nullopt; }
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (!shared_->result) { return std::nullopt; }
    BetaLinkResolved r = std::move(*shared_->result);
    shared_->result.reset();
    return r;
}

BetaMigrationResult exchangePortalToken(const std::string& baseUrl, const std::string& portalToken,
                                        const std::shared_ptr<UploadCancel>& cancel) {
    BetaMigrationResult out;
    if (baseUrl.empty() || portalToken.empty() || !cancel) { return out; }
    // No body content is meaningful to this endpoint - the credential IS the
    // bearer header - but authRequestBounded's POST path still needs a
    // non-empty payload to send, matching postBounded's own "empty means
    // nothing to do" convention elsewhere in this codebase.
    const AuthRequestResult r =
        authRequestBounded("POST", baseUrl + "/api/beta/app-token", portalToken, "{}", cancel);
    if (!r.attempted || r.cancelled) { return out; }
    if (r.status == 404 || r.status == 401) {
        out.outcome = BetaMigrationOutcome::Invalid;
        return out;
    }
    if (r.status < 200 || r.status >= 300) { return out; }
    const nlohmann::json j = nlohmann::json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return out; }
    const auto tokIt = j.find("appToken");
    const auto nameIt = j.find("name");
    if (tokIt == j.end() || !tokIt->is_string()) { return out; }
    const std::string appToken = tokIt->get<std::string>();
    if (!validAppToken(appToken)) { return out; }  // the site owes us its own shape
    out.outcome = BetaMigrationOutcome::Ok;
    out.appToken = appToken;
    if (nameIt != j.end() && nameIt->is_string()) {
        out.name = sanitizeTesterName(nameIt->get<std::string>());
    }
    return out;
}

struct BetaMigrationSender::Shared {
    std::atomic<bool> inFlight{false};
    std::mutex mu;
    std::optional<BetaMigrationResult> result;
};

BetaMigrationSender::~BetaMigrationSender() {
    if (cancel_) { cancel_->cancel(); }
}

bool BetaMigrationSender::busy() const { return shared_ && shared_->inFlight.load(); }

void BetaMigrationSender::send(const std::string& baseUrl, const std::string& portalToken) {
    if (baseUrl.empty() || portalToken.empty() || busy()) { return; }
    cancel_ = std::make_shared<UploadCancel>();
    shared_ = std::make_shared<Shared>();
    shared_->inFlight.store(true);
    auto shared = shared_;
    auto cancel = cancel_;
    std::thread([baseUrl, portalToken, shared, cancel]() {
        BetaMigrationResult r;
        try {
            r = exchangePortalToken(baseUrl, portalToken, cancel);
        } catch (...) {
        }
        if (!cancel->cancelled()) {
            std::lock_guard<std::mutex> lk(shared->mu);
            shared->result = r;
        }
        shared->inFlight.store(false);
    }).detach();
}

std::optional<BetaMigrationResult> BetaMigrationSender::takeResult() {
    if (!shared_) { return std::nullopt; }
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (!shared_->result) { return std::nullopt; }
    BetaMigrationResult r = std::move(*shared_->result);
    shared_->result.reset();
    return r;
}

}  // namespace cascade::core
