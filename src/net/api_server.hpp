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
// THREADS. One IO thread owns every socket. The hub is called from it (a mutex, copies only).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_API_SERVER_HPP
#define CASCADE_NET_API_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    // `authorization` is the value of the Authorization header ("" = none); `client` the peer's address text. Called on the IO thread: it must be quick or answer TooMany.
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
        std::unique_ptr<ws::FrameDecoder> decoder;
        std::uint64_t cursor = 0;       // the newest decode this client has been sent
        std::size_t pending() const { return out.size() - off; }
    };

    void run();
    void acceptAll(std::int64_t now);
    void handleHead(Conn& c, std::int64_t now);
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
    std::vector<Conn> conns_;   // IO thread only

    std::atomic<std::uint64_t> accepted_{0}, refusedFull_{0}, refusedRequests_{0}, wsOpened_{0}, sseOpened_{0}, messages_{0}, errors_{0}, droppedSlow_{0}, droppedStalled_{0}, droppedBad_{0}, authDenied_{0};
};

}  // namespace cascade::net

#endif  // CASCADE_NET_API_SERVER_HPP
