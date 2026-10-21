// server_host.hpp - THE ENGINE'S SERVER HOST (engine extraction stage 5a; docs/api-0.3-additions.md 8.16). The web server and the CAT
// listener become part of the engine (docs/TRANSPORTS.md 2.6): configured by the local operator through FOXAPI_OP_SERVER_CONFIG,
// their configuration part of the engine's half of the config file, their state published through a list and facts every interface
// draws its panels from. This is the TABLE SIDE of that, and only that: the configuration and its validation, the password hashed
// on a worker, the status the listeners will report, and the counters a panel shows live. It owns no socket and starts nothing - the
// listeners themselves move into the engine in stage 5b, and report through setListener()/counters() when they do. Until then the
// window still owns the real servers, so a SERVER_CONFIG changes what the engine records and publishes and nothing that listens.
//
// THREADS. config()/listener()/version()/counters are any-thread; apply(), poll(), load() are the engine's pumping thread's. The
// password is hashed (PBKDF2-HMAC-SHA256, 600 000 iterations, net/web_auth.hpp) on a worker the pass polls: a click handler used to
// run it on the GUI thread, and the pass must never wait for it (a pass stalled past a second unkeys the transmitter's thread).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_SERVER_HOST_HPP
#define CASCADE_ENGINE_SERVER_HOST_HPP

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "net/web_policy.hpp"

namespace cascade::core {
struct AppConfig;
}

namespace cascade::engine {

// The two listeners' configuration: the AppConfig fields web* and cat* (core/config.hpp), as one value.
struct ServerConfig {
    cascade::net::WebServerConfig web;  // enabled, bindAddress, port, username, passwordRecord (never the password)
    struct Cat {
        bool enabled = false;
        bool bindAll = false;  // false: loopback only
        int port = 4532;
        bool operator==(const Cat& o) const { return enabled == o.enabled && bindAll == o.bindAll && port == o.port; }
    } cat;
    bool operator==(const ServerConfig& o) const {
        return web.enabled == o.web.enabled && web.bindAddress == o.web.bindAddress && web.port == o.web.port &&
               web.username == o.web.username && web.passwordRecord == o.web.passwordRecord && cat == o.cat;
    }
};

// ONE FOXAPI_OP_SERVER_CONFIG, parsed. The text is a JSON object:
//   {"web": {"enabled": bool, "bindAddress": "127.0.0.1", "port": 8073, "username": "admin",
//            "password": "...", "passwordRecord": "pbkdf2-sha256$...", "clearPassword": true},
//    "cat": {"enabled": bool, "bindAll": bool, "port": 4532}}
// Every member is optional and means "leave this alone" when absent; an UNKNOWN key at either level is an error (a control request is
// a live instruction, not a file that must survive a newer writer), so is a wrong type, a port outside 1024..65535, a bind address
// the policy does not accept (net::isAcceptableBindAddress), a password under eight characters, a record that does not parse, more
// than one of password / passwordRecord / clearPassword, and an object that says nothing. An empty bindAddress or username resets to
// the default (the config's own rule: a lost value must not widen the binding).
struct ServerConfigRequest {
    std::optional<bool> webEnabled;
    std::optional<std::string> webBindAddress;
    std::optional<int> webPort;
    std::optional<std::string> webUsername;
    std::optional<std::string> webPassword;        // plaintext: hashed on a worker, never stored, never published
    std::optional<std::string> webPasswordRecord;  // an already-serialised record
    bool webClearPassword = false;
    std::optional<bool> catEnabled;
    std::optional<bool> catBindAll;
    std::optional<int> catPort;
};

// False with a sentence in `error` (the result's message) for anything above. Pure.
bool parseServerConfigRequest(const std::string& text, ServerConfigRequest& out, std::string& error);

class ServerHost {
public:
    // What a listener says about itself; reported by whoever runs it (stage 5b: the engine's own web server and CAT listener). The
    // fields change rarely, so they are part of the lists block (and its fingerprint). `refusal` is the sentence a start was refused
    // with, written to be shown verbatim (a bind policy's, a port already in use); it never holds a password. `addresses` are the
    // machine's interface addresses the web server answers on, for the panel's "reachable at" line.
    struct Listener {
        bool running = false;
        int boundPort = 0;
        std::string refusal;
        std::vector<std::string> addresses;
        bool operator==(const Listener& o) const {
            return running == o.running && boundPort == o.boundPort && refusal == o.refusal && addresses == o.addresses;
        }
    };
    // The live figures a panel shows, read at the call (as a recording's bytes are): they move with traffic and must not rebuild the
    // lists. Written by the listener's threads, relaxed.
    struct Counters {
        std::atomic<int> clients{0};          // web: browser sessions; CAT: connected clients
        std::atomic<int> audioListeners{0};   // web only
        std::atomic<std::uint64_t> commands{0};
    };

    struct Outcome {
        std::int32_t status = 0;  // a FOXAPI_* status
        std::string message;
    };

    ServerHost() = default;
    ~ServerHost();
    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    // --- the engine's pumping thread -------------------------------------------------------------------------------------------
    // Applies a parsed request: merges what it names into the configuration (NO_CHANGE when nothing moves and no password job starts),
    // and starts the password's hash on a worker. The answer says so: a password is accepted before its record exists.
    Outcome apply(const ServerConfigRequest& r);
    // Collects the worker's answer; once a pass (Engine::pumpWorkers). Starts the next password waiting behind a finished one.
    void poll();
    // The configuration file's halves. load() is Engine::applyConfig's, store() writes the web*/cat* fields (Engine::fillConfig).
    void load(const cascade::core::AppConfig& cfg);
    void store(cascade::core::AppConfig& cfg) const;

    // --- any thread -------------------------------------------------------------------------------------------------------------
    ServerConfig config() const;
    bool passwordPending() const { return passwordPending_.load(std::memory_order_relaxed); }
    // What moves the lists: the configuration, a listener's report, a password job started or finished. Hashed by the pass every pass
    // (no allocation), and the block is rebuilt when it moves.
    std::uint64_t version() const { return version_.load(std::memory_order_relaxed); }
    Listener web() const;
    Listener cat() const;
    void setWeb(Listener l);
    void setCat(Listener l);
    Counters& webCounters() { return webCounters_; }
    Counters& catCounters() { return catCounters_; }
    const Counters& webCounters() const { return webCounters_; }
    const Counters& catCounters() const { return catCounters_; }

private:
    struct Hashed {
        std::uint64_t generation = 0;
        bool ok = false;
        std::string record;
        std::string error;
    };
    void startHash(std::string plain, std::uint64_t generation);
    void bump() { version_.fetch_add(1, std::memory_order_relaxed); }

    mutable std::mutex m_;
    ServerConfig cfg_;
    Listener web_, cat_;
    Counters webCounters_, catCounters_;
    std::atomic<std::uint64_t> version_{1};
    std::atomic<bool> passwordPending_{false};

    // The password job. Only the pumping thread touches these.
    std::future<Hashed> hashing_;
    std::uint64_t passwordGeneration_ = 0;  // moves with every request that decides the password: an older answer is dropped
    std::optional<std::string> waitingPlain_;  // the newest plaintext asked for while a hash was running
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_SERVER_HOST_HPP
