// server_host.cpp - see server_host.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/server_host.hpp"

#include <algorithm>
#include <chrono>
#include <set>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/diag_log.hpp"
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

ServerHost::ServerHost() {
    // The figures a panel reads, refreshed from the server at every read while it runs (Counters). Set here, before any thread exists.
    webCounters_.sampler = [this](const Counters& c) {
        if (!webServer_.running()) { return; }
        c.clients.store(static_cast<int>(webServer_.sessionCount()), std::memory_order_relaxed);
        c.audioListeners.store(static_cast<int>(webServer_.audioListeners()), std::memory_order_relaxed);
    };
    catCounters_.sampler = [this](const Counters& c) {
        if (!catServer_.running()) { return; }
        c.clients.store(catServer_.clientCount(), std::memory_order_relaxed);
        c.commands.store(catServer_.commandCount(), std::memory_order_relaxed);
    };
    // The spectrum the page long-polls: the contract of Pipeline::getLatestFrame - nothing newer than the caller's cursor is "no frame", so
    // a polling browser never re-fetches a picture it already drew.
    (void)webServer_.setSpectrumProvider([this](cascade::net::SpectrumSnapshot& inOut) {
        std::lock_guard<std::mutex> lock(specM_);
        if (specSeq_ == 0 || inOut.seq >= specSeq_) { return false; }
        inOut.seq = specSeq_;
        inOut.centerHz = specCentreHz_;
        inOut.spanHz = specSpanHz_;
        inOut.dbBins = specBins_;
        return true;
    });
}

ServerHost::~ServerHost() {
    shutdown();
    if (hashing_.valid()) { hashing_.wait(); }
}

void ServerHost::attach(Providers p) {
    if (p.webStatus) { (void)webServer_.setStatusProvider(std::move(p.webStatus)); }
    if (p.webPoll) { (void)webServer_.setStatusPollHook(std::move(p.webPoll)); }
    if (p.catStatus) { catServer_.setStatusProvider(std::move(p.catStatus)); }
}

ServerHost::Pulled ServerHost::pull() {
    Pulled pulled;
    pulled.requests = webServer_.takePendingControls();
    // CAT requests are the SAME ControlRequest type, so they are appended here and applied by the identical code in the engine rather than
    // by a second copy of it. That is the whole reason CAT emits control requests instead of touching the radio: a frequency set over CAT
    // and one set from the browser must do exactly the same thing, including the parts that are easy to forget - telling the RDS decoder to
    // drop the old station, clamping the offset against the live sample rate, and moving the bandwidth to the new mode's default.
    if (catServer_.running()) {
        std::vector<cascade::net::ControlRequest> fromCat = catServer_.takePendingControls();
        pulled.requests.insert(pulled.requests.end(), std::make_move_iterator(fromCat.begin()), std::make_move_iterator(fromCat.end()));
    }
    // READ AFTER THE QUEUES (the order the frame's drain had): the engine ends a key the web remote holds, in the phase that applies these
    // requests, when the server is not running - one that never started, or was disabled in the settings panel. WebServer::stop() also
    // queues a release among the requests above; releasing twice costs nothing.
    pulled.serverRunning = webServer_.running();
    return pulled;
}

void ServerHost::publishSpectrum(std::uint64_t seq, const std::vector<float>& bins, double centreHz, double spanHz) {
    std::lock_guard<std::mutex> lock(specM_);
    // Copy the bins only when the frame actually advanced.
    if (seq == specSeq_) { return; }
    specSeq_ = seq;
    specBins_ = bins;
    specCentreHz_ = centreHz;
    specSpanHz_ = spanHz;
}

// ---- the worker ---------------------------------------------------------------------------------------------------------------------

bool ServerHost::stopping() const {
    std::lock_guard<std::mutex> lk(wm_);
    return wStop_;
}

void ServerHost::kick(bool retry) {
    if (!hosting_.load(std::memory_order_relaxed)) { return; }
    {
        std::lock_guard<std::mutex> lk(wm_);
        if (wStop_) { return; }
        wKick_ = true;
        wRetry_ = wRetry_ || retry;
        if (!worker_.joinable()) { worker_ = std::thread([this] { workerLoop(); }); }
    }
    wcv_.notify_one();
}

void ServerHost::workerLoop() {
    std::unique_lock<std::mutex> lk(wm_);
    for (;;) {
        wcv_.wait(lk, [this] { return wStop_ || wKick_; });
        if (wStop_) { return; }
        const bool retry = wRetry_;
        wKick_ = false;
        wRetry_ = false;
        wBusy_ = true;
        lk.unlock();
        reconcileWeb(retry);
        if (!stopping()) { reconcileCat(retry); }
        lk.lock();
        wBusy_ = false;
        widleCv_.notify_all();
    }
}

bool ServerHost::waitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(wm_);
    return widleCv_.wait_for(lk, timeout, [this] { return !wKick_ && !wBusy_; });
}

namespace {
bool sameWeb(const cascade::net::WebServerConfig& a, const cascade::net::WebServerConfig& b) {
    return a.enabled == b.enabled && a.bindAddress == b.bindAddress && a.port == b.port && a.username == b.username &&
           a.passwordRecord == b.passwordRecord;
}
}  // namespace

// Makes the web server match the configuration as it is now. Start-or-restart is WebServer::start itself (it stops a running server first,
// and drops every session: the binding, and possibly the password, may have just changed). A configuration that was already tried and refused
// is not tried again unless asked (retry): a port something else holds is the operator's to fix, and a loop that hammered it would help no one.
void ServerHost::reconcileWeb(bool retry) {
    const cascade::net::WebServerConfig want = config().web;
    if (!want.enabled) {
        if (webServer_.running()) { webServer_.stop(); }  // (may block: the worker's business, not the pass's)
        webRunCfg_.reset();
        webAttempt_.reset();
        webCounters_.clients.store(0, std::memory_order_relaxed);
        webCounters_.audioListeners.store(0, std::memory_order_relaxed);
        webListening_.store(false, std::memory_order_relaxed);
        setWeb(Listener{});
        return;
    }
    // A PASSWORD IS BEING HASHED (SERVER_CONFIG's "password", on its own worker): the restart waits for its record, so the server comes up with
    // the password the operator just set, once - not first without it (an off-machine bind would be refused for the want of one, and a
    // loopback one served openly for the length of a hash). poll() wakes this when the hash is done. Switching OFF is never held back.
    if (passwordPending_.load(std::memory_order_relaxed)) { return; }
    if (webServer_.running() && webRunCfg_.has_value() && sameWeb(*webRunCfg_, want)) { return; }
    if (!webServer_.running() && webAttempt_.has_value() && sameWeb(*webAttempt_, want) && !retry) { return; }

    std::string error;
    const bool ok = webServer_.start(want, error);
    webAttempt_ = want;
    Listener l;
    if (ok) {
        webRunCfg_ = want;
        l.running = true;
        l.boundPort = webServer_.boundPort();
        // The addresses a phone would type, for the panel's "reachable at" line: only when the binding can be reached from elsewhere (the
        // adapter walk is a syscall nobody needs for a loopback server).
        if (webServer_.decision().reachableOffMachine) { l.addresses = cascade::net::localInterfaceAddresses(); }
        // The PORT and the SCOPE, never the password record and never the bind address's provenance: this line goes in a file the user may send us.
        cascade::core::diagLogf("web: serving on port %d", l.boundPort);
    } else {
        webRunCfg_.reset();
        l.refusal = error;  // the policy's refusal text is written to be shown verbatim; a bind failure's is too
        cascade::core::diagWarnf("web: server refused to start (%s)", error.c_str());
        webCounters_.clients.store(0, std::memory_order_relaxed);
        webCounters_.audioListeners.store(0, std::memory_order_relaxed);
    }
    webListening_.store(ok, std::memory_order_relaxed);
    setWeb(std::move(l));
}

void ServerHost::reconcileCat(bool retry) {
    const ServerConfig::Cat want = config().cat;
    if (!want.enabled) {
        if (catServer_.running()) { catServer_.stop(); }
        catRunCfg_.reset();
        catAttempt_.reset();
        catCounters_.clients.store(0, std::memory_order_relaxed);
        catCounters_.commands.store(0, std::memory_order_relaxed);
        setCat(Listener{});
        return;
    }
    if (catServer_.running() && catRunCfg_.has_value() && *catRunCfg_ == want) { return; }
    if (!catServer_.running() && catAttempt_.has_value() && *catAttempt_ == want && !retry) { return; }
    if (catServer_.running()) { catServer_.stop(); }  // a port or scope change needs a fresh listener
    std::string error;
    const bool ok = catServer_.start(static_cast<std::uint16_t>(want.port), want.bindAll, error);
    catAttempt_ = want;
    Listener l;
    if (ok) {
        catRunCfg_ = want;
        l.running = true;
        l.boundPort = static_cast<int>(catServer_.boundPort());
        // The 120 s freeze that started this whole feature was a CAT client being shut down. Whether one was connected at all is the first
        // question a hang report from that path has to answer.
        cascade::core::diagLogf("cat: listening on port %d (%s)", want.port, want.bindAll ? "all interfaces" : "loopback");
    } else {
        // Left enabled in the config on purpose: the usual cause is a port held by something else (a real rigctld), which the user fixes
        // and retries. Silently turning the setting off would hide that.
        catRunCfg_.reset();
        l.refusal = error;
        cascade::core::diagWarnf("cat: listener refused on port %d (%s)", want.port, error.c_str());
        catCounters_.clients.store(0, std::memory_order_relaxed);
        catCounters_.commands.store(0, std::memory_order_relaxed);
    }
    setCat(std::move(l));
}

void ServerHost::shutdown() {
    std::thread w;
    {
        std::lock_guard<std::mutex> lk(wm_);
        wStop_ = true;
        w = std::move(worker_);
    }
    wcv_.notify_all();
    if (w.joinable() && w.get_id() != std::this_thread::get_id()) { w.join(); }
    // A hard stop each: when it returns no thread of the server runs and every client is closed (what makes destroying them safe).
    webServer_.stop();
    catServer_.stop();
    webRunCfg_.reset();
    catRunCfg_.reset();
    webListening_.store(false, std::memory_order_relaxed);
    setWeb(Listener{});
    setCat(Listener{});
    widleCv_.notify_all();
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
    // The listeners are made to match - and a refused one tried again - whatever the answer: "apply" is the operator asking, and the same
    // configuration said again is how a port that was held and has been freed is retried. A password in flight restarts the web server
    // when its record lands (poll).
    kick(true);
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
    // A NEW PASSWORD: the web server restarts with it, which also ends every session that was signed in under the old one (WebServer::start
    // revokes them all). Off the pass: the worker does it. The worker held the web server's restart back while the hash ran (reconcileWeb), so
    // this is the wake it was waiting for - whether the hash produced a record or failed (the configuration is then acted on as it stands).
    if (!next.has_value()) { kick(false); }
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
    // The restore starts what the file switched on (a refusal - a port held, an off-machine bind with no password - is reported, not
    // fatal), and stops a listener the file does not. Nothing to wake a worker for when both are off and nothing runs.
    if (c.webEnabled || c.catEnabled || webServer_.running() || catServer_.running()) { kick(false); }
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
