// net/api_server.hpp - THE WEBSOCKET AND SSE API (screen lane S4b, G3-31): another program connects, reads what the receiver is doing and, if it wants, retunes it.
//
// ENDPOINTS (HTTP/1.1, GET only; anything else is refused: 405, 404):
//   GET /status   one JSON object, the same the other two carry.
//   GET /events   a Server-Sent Events stream (text/event-stream): "event: status" every second, "event: decode" for each newly decoded line. Read only.
//   GET /ws       a WebSocket (RFC 6455, net/ws_frame.hpp). The server sends the status at once and every second. The client sends JSON text messages:
//                   {"op":"get_status"}  {"op":"list_bookmarks"}  {"op":"recent_decodes","max":N}  {"op":"tune","hz":N}  {"op":"set_mode","mode":"USB"}  {"op":"subscribe","decodes":true|false}
//                 each answered with one JSON message ("id", when the request had one, is echoed). Anything else is answered {"type":"error",...}.
//
// WHAT A CLIENT MAY DO is what net/control_hub.hpp lists: read the status, the bookmarks and the recent decodes; tune; set the mode. There is no operation here that transmits, keys a radio, reads
// or writes a file, changes a setting, starts or stops anything, runs a program or opens a connection: the server has no such code and the hub has no such member.
//
// SAFETY.
//   * Off until started; binds 127.0.0.1 unless allowAllInterfaces (a deliberate setting; 0.0.0.0 is refused otherwise).
//   * Authentication is the caller's: every request that reaches /status, /events or /ws is checked by `auth` (the engine gives it the web page's own account: the same user name and password).
//     With requireAuth a request without credentials is answered 401 whatever `auth` says. A browser cannot use this API: a request that carries an Origin header (every browser page sends one)
//     is refused 403, so a web page cannot reach it by cross-site scripting.
//   * Bounded everywhere: at most maxConnections connections (one past it is closed at once); a request head over 8 KiB is refused 431 and one not finished in headMs is closed; a WebSocket
//     message over maxMessageBytes closes the connection (1009); a malformed frame or text that is not UTF-8 closes it; a JSON message nested deeper than 16 levels is refused before it is parsed;
//     a client that cannot keep up (clientQueueBytes unsent) or stops reading (stallMs) is dropped; ten tuning requests a second at most (the hub's token bucket).
//
// HOST. With no password in force (requireAuth false: this computer only) a request whose Host is not this computer's own name or address (localhost, 127.x, [::1], the bound address) is refused 403: a web page
//   that made its own name point at this computer by DNS rebinding sends its own name in Host. A request that carries an Origin is refused as before. Beyond this computer every request signs in.
// THREADS. One IO thread owns every socket. The hub is called from it (a mutex, copies only). The sign-in (a password check is a long derivation) is done by a second, AUTH WORKER thread, one at a time:
//   the request waits (bounded: maxPendingAuth, then 429), the IO thread goes on serving everybody else. One address may hold at most maxPerAddressHead unfinished requests.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_API_SERVER_HPP
#define CASCADE_NET_API_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/control_hub.hpp"
#include "net/net_sock.hpp"
#include "net/ws_frame.hpp"

namespace cascade::net {

inline constexpr std::uint16_t kApiDefaultPort = 8074;

enum class ApiAuth { Allowed, Denied, TooMany };

struct ApiConfig {
    std::string bindAddress = "127.0.0.1";
    bool allowAllInterfaces = false;
    std::uint16_t port = kApiDefaultPort;       // 0 = the OS picks (tests)
    bool requireAuth = false;                   // a request without an Authorization header is refused 401
    bool checkHost = true;                      // without requireAuth (this computer only): a request whose Host is not this computer's own name or address is refused 403 (DNS rebinding)
    int maxPendingAuth = 8;                     // sign-ins waiting for the worker; one more is answered 429
    int maxPerAddressHead = 4;                  // connections from one address that have not finished their request; one more is closed at once
    int maxConnections = 16;
    std::size_t maxMessageBytes = 4096;         // a WebSocket message from a client
    int headMs = 5000;                          // the request head must be complete within this
    std::size_t clientQueueBytes = 256 * 1024;
    int stallMs = 10000;
    int statusIntervalMs = 1000;
    int pollMs = 20;
};

struct ApiStats {
    std::uint64_t accepted = 0, refusedFull = 0, refusedRequests = 0, wsOpened = 0, sseOpened = 0, messages = 0, errors = 0;
    std::uint64_t droppedSlow = 0, droppedStalled = 0, droppedBad = 0, authDenied = 0;
    int connections = 0;
};

class ApiServer {
public:
    // `authorization` is the value of the Authorization header ("" = none); `client` the peer's address text. Called on the API's own AUTH WORKER thread, never on the IO thread (a password
    // check takes a long derivation and the IO thread owns every socket): it may take its time, but it must not need a lock the thread that stops this server holds.
    using AuthFn = std::function<ApiAuth(const std::string& authorization, const std::string& client)>;

    ApiServer(const ApiConfig& cfg, ControlHub& hub, AuthFn auth) : cfg_(cfg), hub_(hub), auth_(std::move(auth)) {}
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    int port() const { return port_.load(std::memory_order_acquire); }
    ApiStats stats() const;
    // Ends every open stream (WebSocket, SSE) at the next turn of the IO thread (the account the clients signed in with has changed): a client that comes back is judged afresh.
    void closeAllClients() { dropAll_.store(true, std::memory_order_release); }
    // DNS NAMES THAT ARE THIS COMPUTER'S (M9-api-host-rule): the names the web page's own Host rule accepts besides literals (net/web_guard.hpp guardHostNames: this computer's name and name.local, and the
    // hosts of the owner's "Allowed web origins" list). A request whose Host is one of them is answered; every other name is refused as before. IP literals are NOT widened: the interface still takes 127.x,
    // [::1] and its own bound address only (the page's rule also takes any literal). Safe to call from any thread.
    void setHostNames(std::vector<std::string> names);

    // JSON helpers (pure; the tests use them)
    static std::string statusJson(const HubStatus& s);
    static std::string bookmarksJson(const std::vector<HubBookmark>& b);
    static std::string decodeJson(const HubDecode& d);

private:
    enum class Kind { Head, Ws, Sse };
    struct Conn {
        sock::Handle s = sock::kInvalid;
        Kind kind = Kind::Head;
        std::string peer;
        std::string in;                 // the request head so far
        std::string out;                // unsent bytes
        std::size_t off = 0;
        std::int64_t born = 0, lastProgress = 0;
        bool closeAfterSend = false;
        bool wantDecodes = false;
        std::uint64_t id = 0;
        bool authPending = false;       // the sign-in is with the auth worker; the request waits (what the client sends meanwhile is kept, bounded)
        std::string pendTarget, pendHead;
        std::size_t pendEnd = 0;
        std::unique_ptr<ws::FrameDecoder> decoder;
        std::uint64_t cursor = 0;       // the newest decode this client has been sent
        std::size_t pending() const { return out.size() - off; }
    };

    void run();
    void acceptAll(std::int64_t now);
    void handleHead(Conn& c, std::int64_t now);
    void finishRequest(Conn& c, ApiAuth verdict, const std::string& target, const std::string& head, std::size_t end, std::int64_t now);
    void takeAuthResults(std::int64_t now);
    void authLoop();
    bool hostAllowed(const std::string& hostHeader) const;
    void handleWsData(Conn& c, const char* data, std::size_t n, std::int64_t now);
    void answerWs(Conn& c, const std::string& text);
    bool queue(Conn& c, const std::string& bytes);
    bool flush(Conn& c, std::int64_t now);
    void dropConn(std::size_t i, std::atomic<std::uint64_t>* why);
    void respond(Conn& c, int status, const std::string& contentType, const std::string& body, const std::string& extraHeaders = std::string());
    void closeWs(Conn& c, std::uint16_t code, const char* reason);

    ApiConfig cfg_;
    ControlHub& hub_;
    AuthFn auth_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int> port_{0};
    std::atomic<int> connections_{0};
    sock::Handle listener_ = sock::kInvalid;
    std::uint32_t bindIp_ = 0;
    mutable std::mutex hostM_;
    std::vector<std::string> hostNames_;   // lower-case
    std::vector<Conn> conns_;   // IO thread only
    std::uint64_t nextId_ = 1;  // IO thread only

    // the auth worker: sign-ins are checked here, in order, one at a time; verdicts come back through done_
    struct AuthJob { std::uint64_t id = 0; std::string authorization, peer; };
    struct AuthDone { std::uint64_t id = 0; ApiAuth verdict = ApiAuth::Denied; };
    std::thread authThread_;
    std::mutex authM_;
    std::condition_variable authCv_;
    std::deque<AuthJob> jobs_;
    std::vector<AuthDone> done_;
    bool authStop_ = false;
    std::atomic<bool> dropAll_{false};

    std::atomic<std::uint64_t> accepted_{0}, refusedFull_{0}, refusedRequests_{0}, wsOpened_{0}, sseOpened_{0}, messages_{0}, errors_{0}, droppedSlow_{0}, droppedStalled_{0}, droppedBad_{0}, authDenied_{0};
};

}  // namespace cascade::net

#endif  // CASCADE_NET_API_SERVER_HPP
