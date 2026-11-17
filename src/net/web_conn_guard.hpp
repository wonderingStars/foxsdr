// net/web_conn_guard.hpp - the plain web server's bound on a slow client (security review 2, finding R4).
//
// THE HOLE. cpp-httplib times each recv() of a request separately (WebServer sets 10 s) and serves a connection on a worker thread of a small pool. A client that sends a request head one byte
// every 9 s therefore never times out: it holds the worker (and its connection) for ever, and a handful of such clients from one computer keep every worker busy, so the page, the sign-in and the
// listeners' streams stop answering until they let go. The server library has no total time for a request and no hook on an accepted connection, so the two bounds below are put in the one
// place the library lets a program reach: GuardedServer overrides Server::process_and_close_socket (a private virtual: the library's own extension point for the code that serves one accepted
// socket) and is otherwise an httplib::Server in every way.
//
//   1. A TOTAL TIME TO READ ONE REQUEST. Each request is read through the library's own SocketStream constructed WITH its `max_timeout_msec` (the total-time option the library's client side uses and
//      its server side never sets). In the server's stream that total bounds READS only (SocketStream::wait_readable), so the request head and body must arrive within it, from the moment the
//      first byte is there, however slowly they are dripped; a response, a streamed audio or event answer, is written, never read, and is not touched. Over the total the read fails and the
//      connection is closed. (Waiting for the FIRST byte of a request, on a new or a kept-alive connection, is the library's own keep-alive wait, 5 s.)
//   2. A CAP ON CONNECTIONS THAT HAVE NOT YET SHOWN A REQUEST, PER ADDRESS. From accept (and again from the end of each answer) until the request head is complete, a connection is "pending"
//      against its client address. A client address with more than `maxPendingPerAddress` pending connections has the next one closed at once, before a byte is read. A connection that has
//      shown a head leaves the count, so a listener with its streams open never counts against that address; only the connections that say nothing, or drip, do. The loopback address is not
//      counted (the HTTPS relay's connections come from it, and the relay has its own per-address limits; a program on this computer is not a stranger on a network).
//
// WHAT REMAINS (said plainly): an attacker with many different addresses, each holding up to the cap for up to the total time; and a client that sends a whole valid request at the speed it is
// answered (that is a client). The cap and the time are small constants below; the page of an honest browser is unaffected (a browser sends a head in one packet).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_WEB_CONN_GUARD_HPP
#define CASCADE_NET_WEB_CONN_GUARD_HPP

// Must match every other TU in the program that includes httplib.h on non-Windows (see web_server.cpp for the ASan trace this was found from).
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

// The override below copies Server::process_and_close_socket of this exact library version: a different version must be re-read against it, so it does not build until someone has.
static_assert(std::string_view(CPPHTTPLIB_VERSION) == "0.53.1", "net/web_conn_guard.hpp copies httplib 0.53.1's Server::process_and_close_socket: re-read it against the new version");

namespace cascade::net {

struct ConnLimits {
    std::size_t maxPendingPerAddress = 16;                  // connections of one address that have not yet shown a request head
    std::chrono::milliseconds requestRead{20000};           // the most a request (head and body, at most 8 KiB of it) may take to ARRIVE; 0 = no bound (tests only)
    bool exemptLoopback = true;                             // 127.x and ::1 are not counted (tests turn this off to prove the cap on this computer's own address)
};

class GuardedServer : public httplib::Server {
public:
    void setLimits(const ConnLimits& l) { limits_ = l; }
    // Connections closed at once because their address already had `maxPendingPerAddress` connections with no request head.
    std::uint64_t refusedPending() const { return refusedPending_.load(std::memory_order_relaxed); }
    // Called by the server's pre-routing handler (on the worker thread, once the head of a request is complete): this connection is no longer pending. Harmless anywhere else.
    static void headComplete() {
        State* s = current();
        if (s != nullptr && s->counted) { s->owner->release(*s); }
    }

private:
    struct State {
        GuardedServer* owner = nullptr;
        std::string addr;
        bool counted = false;
    };
    static State*& current() {
        static thread_local State* s = nullptr;
        return s;
    }
    static bool isLoopback(const std::string& a) { return a.rfind("127.", 0) == 0 || a == "::1" || a == "localhost"; }

    bool admit(State& s) {
        if (s.counted) { return true; }
        if (limits_.exemptLoopback && isLoopback(s.addr)) { return true; }
        std::lock_guard<std::mutex> lk(m_);
        std::size_t& n = pending_[s.addr];
        if (n >= limits_.maxPendingPerAddress) {
            if (n == 0) { pending_.erase(s.addr); }
            refusedPending_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++n;
        s.counted = true;
        return true;
    }
    void release(State& s) {
        if (!s.counted) { return; }
        std::lock_guard<std::mutex> lk(m_);
        const auto it = pending_.find(s.addr);
        if (it != pending_.end() && --it->second == 0) { pending_.erase(it); }
        s.counted = false;
    }

    bool process_and_close_socket(socket_t sock) override {
        std::string remote_addr;
        int remote_port = 0;
        httplib::detail::get_remote_ip_and_port(sock, remote_addr, remote_port);
        std::string local_addr;
        int local_port = 0;
        httplib::detail::get_local_ip_and_port(sock, local_addr, local_port);

        State st;
        st.owner = this;
        st.addr = remote_addr;
        State* const previous = current();
        current() = &st;
        bool websocket_upgraded = false;
        bool ret = false;
        if (admit(st)) {   // pending from accept: a connection that sends nothing counts until the library's first-byte wait gives up
            const time_t maxMs = static_cast<time_t>(limits_.requestRead.count());
            ret = httplib::detail::process_server_socket_core(
                svr_sock_, sock, keep_alive_max_count_, keep_alive_timeout_sec_, [&](bool close_connection, bool& connection_closed) {
                    if (!admit(st)) {   // a later request of a kept-alive connection counts again until its head is complete
                        connection_closed = true;
                        return false;
                    }
                    // The library's own stream, constructed with its total-time option (reads only; see the header comment).
                    httplib::detail::SocketStream strm(sock, read_timeout_sec_, read_timeout_usec_, write_timeout_sec_, write_timeout_usec_, maxMs, std::chrono::steady_clock::now());
                    strm.set_readable_hint();   // process_server_socket_core only gets here once keep_alive() has seen the socket go readable
                    const bool r = process_request(strm, remote_addr, remote_port, local_addr, local_port, close_connection, connection_closed, nullptr, &websocket_upgraded);
                    release(st);
                    // A request that could not be READ (the total time was reached, a recv timed out, the peer went) ends the connection: the library answers it with a 400 and would
                    // otherwise go back to wait for the next request on the same connection, which a client that keeps dripping would keep for ever.
                    if (strm.get_error() != httplib::Error::Success) { connection_closed = true; }
                    return r;
                });
        }
        release(st);   // BEFORE the socket is closed
        current() = previous;
        httplib::detail::drain_and_close_socket(sock);
        return ret;
    }

    ConnLimits limits_;
    std::mutex m_;
    std::map<std::string, std::size_t> pending_;
    std::atomic<std::uint64_t> refusedPending_{0};
};

}  // namespace cascade::net

#endif  // CASCADE_NET_WEB_CONN_GUARD_HPP
