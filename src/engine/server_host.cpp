// server_host.cpp - see server_host.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/server_host.hpp"

#include <algorithm>
#include <chrono>
#include <set>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "foxsdr_api.h"
#include "net/web_auth.hpp"

namespace cascade::engine {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxFieldChars = 256;  // an address, an account name: nothing here is longer
constexpr std::size_t kMaxRecordChars = 512;
constexpr std::size_t kMaxPasswordChars = 256;

// Every key of `j` must be in `known`.
bool onlyKnown(const json& j, const std::set<std::string>& known, const char* where, std::string& error) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (known.count(it.key()) == 0) {
            error = std::string("unknown key \"") + it.key() + "\" in " + where;
            return false;
        }
    }
    return true;
}

bool readBool(const json& j, const char* key, std::optional<bool>& dst, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end()) { return true; }
    if (!it->is_boolean()) {
        error = std::string(key) + " must be true or false";
        return false;
    }
    dst = it->get<bool>();
    return true;
}

bool readPort(const json& j, const char* key, std::optional<int>& dst, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end()) { return true; }
    if (!it->is_number_integer()) {
        error = std::string(key) + " must be a whole number";
        return false;
    }
    const long long v = it->get<long long>();
    if (v < cascade::net::kMinPort || v > cascade::net::kMaxPort) {
        error = std::string(key) + " must be between " + std::to_string(cascade::net::kMinPort) + " and " +
                std::to_string(cascade::net::kMaxPort);
        return false;
    }
    dst = static_cast<int>(v);
    return true;
}

bool readText(const json& j, const char* key, std::size_t maxChars, std::optional<std::string>& dst, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end()) { return true; }
    if (!it->is_string()) {
        error = std::string(key) + " must be a string";
        return false;
    }
    std::string v = it->get<std::string>();
    if (v.size() > maxChars) {
        error = std::string(key) + " is too long";
        return false;
    }
    dst = std::move(v);
    return true;
}

}  // namespace

bool parseServerConfigRequest(const std::string& text, ServerConfigRequest& out, std::string& error) {
    out = ServerConfigRequest{};
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the configuration must be a JSON object";
        return false;
    }
    if (!onlyKnown(j, {"web", "cat"}, "the configuration", error)) { return false; }
    bool said = false;
    if (const auto w = j.find("web"); w != j.end()) {
        if (!w->is_object()) {
            error = "web must be an object";
            return false;
        }
        if (!onlyKnown(*w, {"enabled", "bindAddress", "port", "username", "password", "passwordRecord", "clearPassword"}, "web",
                       error)) {
            return false;
        }
        if (!readBool(*w, "enabled", out.webEnabled, error) || !readPort(*w, "port", out.webPort, error) ||
            !readText(*w, "bindAddress", kMaxFieldChars, out.webBindAddress, error) ||
            !readText(*w, "username", kMaxFieldChars, out.webUsername, error) ||
            !readText(*w, "password", kMaxPasswordChars, out.webPassword, error) ||
            !readText(*w, "passwordRecord", kMaxRecordChars, out.webPasswordRecord, error)) {
            return false;
        }
        std::optional<bool> clear;
        if (!readBool(*w, "clearPassword", clear, error)) { return false; }
        if (clear.has_value() && !*clear) {
            error = "clearPassword can only be true";
            return false;
        }
        out.webClearPassword = clear.value_or(false);
        // An EMPTY address or account name resets to the default (core/config.hpp's rule); a literal the policy would never bind is
        // refused here rather than stored, so the file can never hold one this op wrote.
        if (out.webBindAddress.has_value()) {
            if (out.webBindAddress->empty()) {
                out.webBindAddress = cascade::net::WebServerConfig{}.bindAddress;
            } else if (!cascade::net::isAcceptableBindAddress(*out.webBindAddress)) {
                error = "bindAddress is not an address the server accepts (127.0.0.1, 0.0.0.0, or the literal address of one interface)";
                return false;
            }
        }
        if (out.webUsername.has_value() && out.webUsername->empty()) {
            out.webUsername = cascade::net::WebServerConfig{}.username;
        }
        const int forms = (out.webPassword.has_value() ? 1 : 0) + (out.webPasswordRecord.has_value() ? 1 : 0) +
                          (out.webClearPassword ? 1 : 0);
        if (forms > 1) {
            error = "give one of password, passwordRecord and clearPassword";
            return false;
        }
        if (out.webPassword.has_value() && out.webPassword->size() < cascade::net::kMinPasswordLength) {
            error = "the password must be at least " + std::to_string(cascade::net::kMinPasswordLength) + " characters";
            return false;
        }
        if (out.webPasswordRecord.has_value()) {
            cascade::net::PasswordRecord rec;
            std::string why;
            if (!cascade::net::PasswordRecord::parse(*out.webPasswordRecord, rec, why)) {
                error = "passwordRecord is not a password record (" + why + ")";
                return false;
            }
        }
        said = said || !w->empty();
    }
    if (const auto c = j.find("cat"); c != j.end()) {
        if (!c->is_object()) {
            error = "cat must be an object";
            return false;
        }
        if (!onlyKnown(*c, {"enabled", "bindAll", "port"}, "cat", error)) { return false; }
        if (!readBool(*c, "enabled", out.catEnabled, error) || !readBool(*c, "bindAll", out.catBindAll, error) ||
            !readPort(*c, "port", out.catPort, error)) {
            return false;
        }
        said = said || !c->empty();
    }
    if (!said) {
        error = "the configuration says nothing";  // "{}" is a client that meant to say something and did not
        return false;
    }
    return true;
}

ServerHost::~ServerHost() {
    if (hashing_.valid()) { hashing_.wait(); }
}

ServerHost::Outcome ServerHost::apply(const ServerConfigRequest& r) {
    Outcome out;
    out.status = FOXAPI_OK;
    bool changed = false;
    bool passwordAsked = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto set = [&](auto& dst, const auto& v) {
            if (!(dst == v)) {
                dst = v;
                changed = true;
            }
        };
        if (r.webEnabled) { set(cfg_.web.enabled, *r.webEnabled); }
        if (r.webBindAddress) { set(cfg_.web.bindAddress, *r.webBindAddress); }
        if (r.webPort) { set(cfg_.web.port, *r.webPort); }
        if (r.webUsername) { set(cfg_.web.username, *r.webUsername); }
        if (r.catEnabled) { set(cfg_.cat.enabled, *r.catEnabled); }
        if (r.catBindAll) { set(cfg_.cat.bindAll, *r.catBindAll); }
        if (r.catPort) { set(cfg_.cat.port, *r.catPort); }
        if (r.webPasswordRecord) {
            ++passwordGeneration_;  // whatever was being hashed is no longer the answer
            waitingPlain_.reset();
            set(cfg_.web.passwordRecord, *r.webPasswordRecord);
        }
        if (r.webClearPassword) {
            ++passwordGeneration_;
            waitingPlain_.reset();
            set(cfg_.web.passwordRecord, std::string());
        }
        if (r.webPassword) {
            ++passwordGeneration_;
            passwordAsked = true;
            if (hashing_.valid()) {
                waitingPlain_ = *r.webPassword;  // one hash at a time; the newest wish is the one that runs next
            } else {
                // (started outside the lock below)
            }
        }
    }
    if (passwordAsked) {
        if (!hashing_.valid()) { startHash(*r.webPassword, passwordGeneration_); }
        passwordPending_.store(true, std::memory_order_relaxed);
        changed = true;
        out.message = "accepted; the password is being hashed and its record will follow";
    }
    if (changed) {
        bump();
    } else {
        out.status = FOXAPI_NO_CHANGE;
        out.message = "the configuration already says that";
    }
    return out;
}

void ServerHost::startHash(std::string plain, std::uint64_t generation) {
    hashing_ = std::async(std::launch::async, [plain = std::move(plain), generation]() mutable {
        Hashed h;
        h.generation = generation;
        cascade::net::PasswordRecord rec;
        h.ok = cascade::net::hashPassword(plain, rec, h.error);
        if (h.ok) { h.record = rec.serialize(); }
        std::fill(plain.begin(), plain.end(), '\0');  // the plaintext does not outlive its hash
        return h;
    });
}

void ServerHost::poll() {
    if (!hashing_.valid()) { return; }
    if (hashing_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return; }
    Hashed h = hashing_.get();
    bool moved = false;
    std::optional<std::string> next;
    {
        std::lock_guard<std::mutex> lk(m_);
        // An older answer than the newest request about the password is dropped: the request that replaced it decided otherwise.
        if (h.ok && h.generation == passwordGeneration_ && cfg_.web.passwordRecord != h.record) {
            cfg_.web.passwordRecord = h.record;
            moved = true;
        }
        next = std::move(waitingPlain_);
        waitingPlain_.reset();
    }
    if (next.has_value()) {
        startHash(std::move(*next), passwordGeneration_);
    } else {
        passwordPending_.store(false, std::memory_order_relaxed);
    }
    (void)moved;
    bump();  // the pending flag moved (and the record, when it did)
}

void ServerHost::load(const cascade::core::AppConfig& c) {
    {
        std::lock_guard<std::mutex> lk(m_);
        cfg_.web.enabled = c.webEnabled;
        cfg_.web.bindAddress = c.webBindAddress;
        cfg_.web.port = c.webPort;
        cfg_.web.username = c.webUsername;
        cfg_.web.passwordRecord = c.webPasswordRecord;
        cfg_.cat.enabled = c.catEnabled;
        cfg_.cat.bindAll = c.catBindAll;
        cfg_.cat.port = c.catPort;
    }
    bump();
}

void ServerHost::store(cascade::core::AppConfig& c) const {
    std::lock_guard<std::mutex> lk(m_);
    c.webEnabled = cfg_.web.enabled;
    c.webBindAddress = cfg_.web.bindAddress;
    c.webPort = cfg_.web.port;
    c.webUsername = cfg_.web.username;
    c.webPasswordRecord = cfg_.web.passwordRecord;
    c.catEnabled = cfg_.cat.enabled;
    c.catBindAll = cfg_.cat.bindAll;
    c.catPort = cfg_.cat.port;
}

ServerConfig ServerHost::config() const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_;
}

ServerHost::Listener ServerHost::web() const {
    std::lock_guard<std::mutex> lk(m_);
    return web_;
}

ServerHost::Listener ServerHost::cat() const {
    std::lock_guard<std::mutex> lk(m_);
    return cat_;
}

void ServerHost::setWeb(Listener l) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (web_ == l) { return; }
        web_ = std::move(l);
    }
    bump();
}

void ServerHost::setCat(Listener l) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (cat_ == l) { return; }
        cat_ = std::move(l);
    }
    bump();
}

}  // namespace cascade::engine
