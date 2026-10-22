// server_host.hpp - THE ENGINE'S SERVER HOST (engine extraction stage 5; docs/api-0.3-additions.md 8.16, docs/engine-stage4.md 6.8 and 6.10).
// The web server and the CAT listener are part of the engine (docs/TRANSPORTS.md 2.6): configured by the local operator through
// FOXAPI_OP_SERVER_CONFIG, their configuration part of the engine's half of the config file, their state published through a list and facts
// every interface draws its panels from.
//
// STAGE 5a built the TABLE SIDE: the configuration and its validation, the password hashed on a worker, the status the listeners report,
// and the counters a panel shows live. STAGE 5b (this file now) gives the host the SOCKETS: it owns the net::WebServer and the
// net::CatServer, starts and stops them on a worker of its own to match the configuration, and reports what they did through
// setWeb()/setCat(). The window no longer owns either server; it configures them (SERVER_CONFIG), reads them (list 49, the facts) and feeds
// them the data it still produces (audio, pictures, tiles, the spectrum - until 5e).
//
// THREADS. config()/listener()/version()/counters are any-thread; apply(), poll(), load(), pull() are the engine's pumping thread's. The
// password is hashed (PBKDF2-HMAC-SHA256, 600 000 iterations, net/web_auth.hpp) on a worker the pass polls, and the SERVERS are started
// and stopped on a WORKER THREAD of their own - never in the pass: WebServer::stop() can block for as long as the read timeout (10 s), and
// a pass stalled past a second unkeys the transmitter's thread. The pass only records a wish (one lock, one notify) and the worker makes it
// true: it takes the configuration as it is NOW each time it wakes, so a burst of changes is one restart, and a change that lands during a
// start is acted on when that start returns. The servers' own threads call only the providers attach() was given.
//
// TEARDOWN. shutdown() joins the worker, then stops both servers (each stop is a hard stop: no thread of the server runs after it
// returns). Engine::shutdownQuiesce calls it FIRST, before the control thread stops; the destructor calls it again, and the host is the
// last member the engine declares so it dies before the ApiHost the providers reach.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_SERVER_HOST_HPP
#define CASCADE_ENGINE_SERVER_HOST_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/cat_server.hpp"
#include "net/web_policy.hpp"
#include "net/web_server.hpp"

namespace cascade::core {
struct AppConfig;
}

namespace cascade::engine {

class ApiHost;

// A CAT CONNECTION'S SESSION ON THE ENGINE (stage 5c): opens ApiHost::openInternalSession(InternalKind::Cat, label) and wraps it in the
// net::CatSession the CAT server speaks - read_state for the six figures, submit for a request's commands, poll_results for their results,
// close_session in the destructor - every call the C table's own, on the FoxSession* it returns. Null when the engine will not give a
// session (FOXAPI_LIMIT: 64 in all, 16 REMOTE; an internal session counts like any other). The host must outlive the session (the engine's
// ServerHost is destroyed first, and stops the CAT server - which closes every session - before that).
std::unique_ptr<cascade::net::CatSession> openCatSession(ApiHost& api, const std::string& label);

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
    // What a listener says about itself; reported by whoever runs it (stage 5b: this host's worker). The fields change rarely, so they
    // are part of the lists block (and its fingerprint). `refusal` is the sentence a start was refused with, written to be shown
    // verbatim (a bind policy's, a port already in use); it never holds a password. `addresses` are the machine's interface addresses the
    // web server answers on, for the panel's "reachable at" line.
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
    // lists. Relaxed atomics. While a server RUNS the host's sampler refreshes them from the server at every read (sample(), called by
    // the list read on the reader's thread - the old panel read the same figures off the server at its draw); while it does not they hold
    // whatever was last stored (zero after a stop; the table's own tests store theirs).
    struct Counters {
        mutable std::atomic<int> clients{0};          // web: browser sessions; CAT: connected clients
        mutable std::atomic<int> audioListeners{0};   // web only
        mutable std::atomic<std::uint64_t> commands{0};
        std::function<void(const Counters&)> sampler;  // set once, by the host's constructor, before any thread exists
        void sample() const {
            if (sampler) { sampler(*this); }
        }
    };

    struct Outcome {
        std::int32_t status = 0;  // a FOXAPI_* status
        std::string message;
    };

    ServerHost();
    ~ServerHost();
    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    // --- the providers (stage 5b) ------------------------------------------------------------------------------------------------
    // What the servers answer from, installed ONCE by the engine's constructor before any start (a server refuses to change them while it
    // runs): the web page's status (the whole block of one publish), its keep-alive (any authorised GET /api/status - the browser's poll,
    // never a PTT re-assertion) and the way a CAT connection gets its session (since 5c CAT answers from, and commands through, a session of
    // its own per connection - see openCatSession). The spectrum the page long-polls is the host's own (publishSpectrum, below).
    struct Providers {
        cascade::net::WebServer::StatusProvider webStatus;
        cascade::net::WebServer::PollHook webPoll;
        cascade::net::CatSessionOpener catSession;
    };
    void attach(Providers p);

    // --- the old private chain, owned here (stage 5b; 5c took CAT off it, 5d replaces the rest with a session) ---------------------
    // What the engine's pass pulls (Engine::pumpRemoteControls): the browser's queued requests, and whether the web server is running AT
    // THE MOMENT OF THE PULL, read after the queue - the standing condition on the remote key (docs/engine-stage3.md OPEN 7 (c)). CAT is
    // not here any more: its connections hold sessions, whose commands the pass applies in the internal tier right after this pull's
    // (ApiHost::applyTail), so the order the pull always gave - the web's requests, then CAT's - is the order of the phase itself.
    struct Pulled {
        std::vector<cascade::net::ControlRequest> requests;
        bool serverRunning = false;
    };
    Pulled pull();

    // --- the data the window still feeds the web page (stage 5b; END: 5e, which reads them from the table) -----------------------------
    // The spectrum frame the browser long-polls, labelled with the centre and the span the look carries; copied only when the sequence
    // moved. (The audio, the pictures and the tiles go straight to the web server's own thread-safe stores: the engine's shell forwards.)
    void publishSpectrum(std::uint64_t seq, const std::vector<float>& bins, double centreHz, double spanHz);

    // --- the servers --------------------------------------------------------------------------------------------------------------
    cascade::net::WebServer& webServer() { return webServer_; }
    cascade::net::CatServer& catServer() { return catServer_; }
    // Whether the web server is listening - lock-free; what the receiver state's WEB_LISTENING bit carries.
    bool webListening() const { return webListening_.load(std::memory_order_relaxed); }

    // Whether this host starts the listeners it is configured for (true; false for the hermetic engines of the tests - see
    // ApiHost::hostsServers). Off, the configuration is recorded and published and nothing is bound; set before the pass runs.
    void setHosting(bool on) { hosting_.store(on, std::memory_order_relaxed); }

    // Stops the worker and both servers, for good (a later wish is ignored). Any thread but the worker's; idempotent. Blocks for as long
    // as a server's stop does (a browser holding a request open: up to the read timeout).
    void shutdown();
    // TESTS: waits until the worker has nothing pending and is not inside a start or a stop; false on timeout.
    bool waitIdle(std::chrono::milliseconds timeout);

    // --- the engine's pumping thread -------------------------------------------------------------------------------------------
    // Applies a parsed request: merges what it names into the configuration (NO_CHANGE when nothing moves and no password job starts),
    // and starts the password's hash on a worker. The answer says so: a password is accepted before its record exists. Whatever the
    // answer, the worker is asked to make the listeners match (and a listener that was refused is tried again: a request that says
    // nothing new is still the operator asking).
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

    // The worker (stage 5b). kick() is the pass's whole part: a flag under a small lock, a notify, and - the first time - the thread.
    void kick(bool retry);
    void workerLoop();
    void reconcileWeb(bool retry);
    void reconcileCat(bool retry);
    bool stopping() const;

    mutable std::mutex m_;
    ServerConfig cfg_;
    Listener web_, cat_;
    Counters webCounters_, catCounters_;
    std::atomic<std::uint64_t> version_{1};
    std::atomic<bool> passwordPending_{false};

    // The servers and what the worker last did with them. The servers are thread-safe objects; the "last" records are the worker's alone.
    cascade::net::WebServer webServer_;
    cascade::net::CatServer catServer_;
    std::optional<cascade::net::WebServerConfig> webRunCfg_;   // what the running web server was started with
    std::optional<cascade::net::WebServerConfig> webAttempt_;  // what the last start was tried with (refused or not)
    std::optional<ServerConfig::Cat> catRunCfg_;
    std::optional<ServerConfig::Cat> catAttempt_;
    std::atomic<bool> webListening_{false};
    std::atomic<bool> hosting_{true};

    // The spectrum the page long-polls.
    mutable std::mutex specM_;
    std::vector<float> specBins_;
    std::uint64_t specSeq_ = 0;
    double specCentreHz_ = 0.0;
    double specSpanHz_ = 0.0;

    // The worker's wish. wm_ guards the flags; it is never held across a call into a server or into m_.
    mutable std::mutex wm_;
    std::condition_variable wcv_;
    std::condition_variable widleCv_;
    std::thread worker_;
    bool wKick_ = false;
    bool wRetry_ = false;  // a wish that asks a refused listener to try again (a SERVER_CONFIG said, even to say nothing new)
    bool wBusy_ = false;
    bool wStop_ = false;

    // The password job. Only the pumping thread touches these.
    std::future<Hashed> hashing_;
    std::uint64_t passwordGeneration_ = 0;  // moves with every request that decides the password: an older answer is dropped
    std::optional<std::string> waitingPlain_;  // the newest plaintext asked for while a hash was running
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_SERVER_HOST_HPP
