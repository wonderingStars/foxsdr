// test_rtl_tcp_source.cpp - the rtl_tcp client, proven byte for byte against a
// fake rtl_tcp server on a real loopback socket.
//
// WHERE THE EXPECTATIONS COME FROM, AND WHAT THEY CANNOT PROVE. The suite needs
// no rtl_tcp server and no dongle behind one (a real server was checked by hand
// on 2026-10-06, see src/source/rtl_tcp_source.hpp). The wire format the
// fake speaks is the one written down in src/source/rtl_tcp_source.hpp (the
// protocol's public description: a 12-byte header, 5-byte big-endian
// commands, an endless stream of unsigned 8-bit I/Q), so a test that only
// agreed with the driver would prove nothing. Two measures against that: every
// command number and every byte below is a LITERAL (never the driver's own
// named constants), written out by hand from the description, and the fake is
// written independently of the driver. What this suite does NOT prove: that a
// real rtl_tcp server (and the servers that imitate it) accept exactly these
// bytes and send exactly this header. That was checked by hand on 2026-10-06
// against radioconda's rtl_tcp.exe serving an RTL2838 (R820T): the header was
// read, the stream arrived at 2.4 MS/s and the fault was latched when the
// server exited. Servers that only imitate rtl_tcp have not been tried.
//
// THE FAKE IS A REAL SERVER ON A REAL SOCKET: 127.0.0.1, an ephemeral port, its
// own accept and per-connection threads, non-blocking sends gated by select so
// a client that is not reading yet (the time between open() and start()) can
// never wedge the thread that has to read its commands.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "gui/bias_tee.hpp"
#include "source/iiod_client.hpp"
#include "source/rtl_tcp_source.hpp"
#include "test_check.hpp"

using cascade::source::GainUnit;
using cascade::source::RtlTcpSource;
namespace health = cascade::core::health;

namespace {

// --- sockets for the fake -------------------------------------------------

#if defined(_WIN32)
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
void closeSock(Sock s) {
    if (s != kBadSock) { ::closesocket(s); }
}
bool startNetwork() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
void setNonBlocking(Sock s) {
    u_long mode = 1;
    ::ioctlsocket(s, FIONBIO, &mode);
}
bool wouldBlock() { return ::WSAGetLastError() == WSAEWOULDBLOCK; }
int sendBytes(Sock s, const char* p, int n) { return ::send(s, p, n, 0); }
int recvBytes(Sock s, char* p, int n) { return ::recv(s, p, n, 0); }
using SockLen = int;
#else
using Sock = int;
constexpr Sock kBadSock = -1;
void closeSock(Sock s) {
    if (s != kBadSock) { ::close(s); }
}
bool startNetwork() { return true; }
void setNonBlocking(Sock s) { ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
int sendBytes(Sock s, const char* p, int n) {
    return static_cast<int>(::send(s, p, static_cast<std::size_t>(n), MSG_NOSIGNAL));
}
int recvBytes(Sock s, char* p, int n) {
    return static_cast<int>(::recv(s, p, static_cast<std::size_t>(n), 0));
}
using SockLen = socklen_t;
#endif

using Bytes = std::vector<std::uint8_t>;

// One command, spelled out: the id, then the value big-endian. Used for the
// EXPECTATIONS, from literal numbers in the test body.
Bytes cmd(std::uint8_t id, std::uint32_t value) {
    return {id,
            static_cast<std::uint8_t>(value >> 24),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value)};
}

std::string hex(const Bytes& b) {
    std::string s;
    char buf[8];
    for (const std::uint8_t c : b) {
        std::snprintf(buf, sizeof(buf), "%02X ", c);
        s += buf;
    }
    return s;
}

// --- the fake rtl_tcp server ----------------------------------------------
//
// What it does per connection, in order: sends its header (or something else,
// per `header`), then serves two things at once from one select loop - reads
// five-byte commands into a log, and writes an endless ramp of bytes (byte n
// has the value n & 0xFF, so a lost chunk, a repeated one or an I/Q swap shows
// up as a mismatch at a named sample) paced at a few hundred kilobytes a
// second so a test that is not draining never overflows the client's ring.
class FakeRtlTcp {
public:
    enum class Header {
        Normal,          // "RTL0", tunerType, gainCount
        BadMagic,        // twelve bytes that are not an rtl_tcp header
        Silent,          // accept, say nothing
        CloseImmediately // accept, hang up
    };

    ~FakeRtlTcp() { stop(); }

    bool start() {
        if (!startNetwork()) { return false; }
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kBadSock) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        SockLen len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return false;
        }
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listener_, 8) != 0) { return false; }
        run_.store(true);
        acceptor_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        run_.store(false);
        if (acceptor_.joinable()) { acceptor_.join(); }
        for (std::thread& t : workers_) {
            if (t.joinable()) { t.join(); }
        }
        workers_.clear();
        if (listener_ != kBadSock) {
            closeSock(listener_);
            listener_ = kBadSock;
        }
    }

    std::uint16_t port() const { return port_; }
    std::string args() const { return "rtltcp=127.0.0.1:" + std::to_string(port_); }

    // Settings, read per connection.
    std::atomic<std::uint32_t> tunerType{5};
    std::atomic<std::uint32_t> gainCount{29};
    std::atomic<Header> header{Header::Normal};
    // The stream waits for this: a test that wants to watch bytes arrive in
    // small pieces opens the gate only AFTER the client's reader is running.
    std::atomic<bool> streamEnabled{true};
    // The first 64 stream bytes go out ONE AT A TIME, 2 ms apart, so a reader
    // that is already waiting sees a receive of a single byte - half a pair -
    // on every other call.
    std::atomic<bool> trickleFirst{false};
    // Close the connection (orderly) once this many stream bytes have gone.
    std::atomic<long> dieAfterBytes{-1};
    // Go quiet (connection kept) once this many stream bytes have gone.
    std::atomic<long> stallAfterBytes{-1};

    std::vector<Bytes> commands(std::size_t connection) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (connection >= log_.size()) { return {}; }
        return log_[connection];
    }
    std::size_t connections() {
        std::lock_guard<std::mutex> lk(mutex_);
        return log_.size();
    }
    // Connections the CLIENT closed (the server's recv saw an orderly close).
    int clientClosed() const { return clientClosed_.load(); }

private:
    void acceptLoop() {
        while (run_.load()) {
            // SELECT, THEN ACCEPT: SO_RCVTIMEO does not bound accept() on
            // Windows (see tests/test_pluto_source.cpp, which hung its whole
            // suite on exactly that).
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listener_, &rfds);
            timeval tv{};
            tv.tv_usec = 50 * 1000;
#if defined(_WIN32)
            const int ready = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
            const int ready = ::select(listener_ + 1, &rfds, nullptr, nullptr, &tv);
#endif
            if (ready <= 0) { continue; }
            sockaddr_in from{};
            SockLen len = sizeof(from);
            const Sock s = ::accept(listener_, reinterpret_cast<sockaddr*>(&from), &len);
            if (s == kBadSock) { continue; }
            std::size_t index = 0;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                index = log_.size();
                log_.emplace_back();
            }
            workers_.emplace_back([this, s, index] { serve(s, index); });
        }
    }

    // A blocking-in-effect send of a small buffer on a non-blocking socket.
    bool sendFully(Sock s, const Bytes& b) {
        std::size_t sent = 0;
        while (sent < b.size() && run_.load()) {
            const int r = sendBytes(s, reinterpret_cast<const char*>(b.data()) + sent,
                                    static_cast<int>(b.size() - sent));
            if (r > 0) {
                sent += static_cast<std::size_t>(r);
            } else if (r < 0 && wouldBlock()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                return false;
            }
        }
        return sent == b.size();
    }

    void serve(Sock s, std::size_t index) {
        setNonBlocking(s);
        const Header h = header.load();
        if (h == Header::CloseImmediately) {
            closeSock(s);
            return;
        }
        if (h == Header::BadMagic) {
            const std::string junk = "HTTP/1.1 400 Bad Request\r\n";
            sendFully(s, Bytes(junk.begin(), junk.end()));
            // Keep the socket open: it is the client that must decide this is
            // not rtl_tcp, not a close that tells it so.
        } else if (h == Header::Normal) {
            Bytes hdr = {'R', 'T', 'L', '0'};
            const std::uint32_t t = tunerType.load();
            const std::uint32_t g = gainCount.load();
            for (const std::uint32_t v : {t, g}) {
                hdr.push_back(static_cast<std::uint8_t>(v >> 24));
                hdr.push_back(static_cast<std::uint8_t>(v >> 16));
                hdr.push_back(static_cast<std::uint8_t>(v >> 8));
                hdr.push_back(static_cast<std::uint8_t>(v));
            }
            if (!sendFully(s, hdr)) {
                closeSock(s);
                return;
            }
        }

        Bytes pending;           // command bytes not yet a whole frame
        long streamed = 0;       // stream bytes sent so far
        std::size_t chunkNo = 0;
        const std::size_t chunks[] = {997, 1501, 3, 1, 2049, 4095, 5, 7};
        bool dead = false;
        while (run_.load() && !dead) {
            const bool streaming = h == Header::Normal && streamEnabled.load();
            const long die = dieAfterBytes.load();
            const long stall = stallAfterBytes.load();
            const bool wantWrite = streaming && !(stall >= 0 && streamed >= stall);

            fd_set rfds;
            fd_set wfds;
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(s, &rfds);
            if (wantWrite) { FD_SET(s, &wfds); }
            timeval tv{};
            tv.tv_usec = 5 * 1000;
#if defined(_WIN32)
            const int ready = ::select(0, &rfds, wantWrite ? &wfds : nullptr, nullptr, &tv);
#else
            const int ready =
                ::select(s + 1, &rfds, wantWrite ? &wfds : nullptr, nullptr, &tv);
#endif
            if (ready < 0) { break; }

            if (FD_ISSET(s, &rfds)) {
                char buf[256];
                const int r = recvBytes(s, buf, sizeof(buf));
                if (r == 0) {
                    clientClosed_.fetch_add(1);
                    break;
                }
                if (r < 0) {
                    if (!wouldBlock()) { break; }
                } else {
                    pending.insert(pending.end(), buf, buf + r);
                    while (pending.size() >= 5) {
                        Bytes frame(pending.begin(), pending.begin() + 5);
                        pending.erase(pending.begin(), pending.begin() + 5);
                        std::lock_guard<std::mutex> lk(mutex_);
                        log_[index].push_back(std::move(frame));
                    }
                }
            }

            if (wantWrite && FD_ISSET(s, &wfds)) {
                std::size_t n;
                if (trickleFirst.load() && streamed < 64) {
                    n = 1;
                } else {
                    n = chunks[chunkNo++ % (sizeof(chunks) / sizeof(chunks[0]))];
                }
                if (die >= 0 && streamed + static_cast<long>(n) > die) {
                    n = static_cast<std::size_t>(die - streamed);
                }
                if (n > 0) {
                    Bytes out(n);
                    for (std::size_t i = 0; i < n; ++i) {
                        out[i] = static_cast<std::uint8_t>((streamed + static_cast<long>(i)) & 0xFF);
                    }
                    std::size_t sent = 0;
                    while (sent < n && run_.load()) {
                        const int r =
                            sendBytes(s, reinterpret_cast<const char*>(out.data()) + sent,
                                      static_cast<int>(n - sent));
                        if (r > 0) {
                            sent += static_cast<std::size_t>(r);
                        } else if (r < 0 && wouldBlock()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        } else {
                            dead = true;
                            break;
                        }
                    }
                    streamed += static_cast<long>(sent);
                }
                if (die >= 0 && streamed >= die) { break; }  // the server goes away
                std::this_thread::sleep_for(std::chrono::milliseconds(trickleFirst.load() &&
                                                                              streamed <= 64
                                                                          ? 2
                                                                          : 1));
            }
        }
        closeSock(s);
    }

    Sock listener_ = kBadSock;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::vector<std::vector<Bytes>> log_;
    std::atomic<int> clientClosed_{0};
};

template <typename Fn>
bool waitFor(Fn fn, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

bool waitForCommands(FakeRtlTcp& d, std::size_t conn, std::size_t count,
                     std::chrono::milliseconds bound = std::chrono::milliseconds(3000)) {
    return waitFor([&] { return d.commands(conn).size() >= count; }, bound);
}

// Reads until `n` samples or the bound.
std::vector<std::complex<float>> readN(RtlTcpSource& src, std::size_t n,
                                       std::chrono::milliseconds bound) {
    std::vector<std::complex<float>> out;
    std::vector<std::complex<float>> buf(4096);
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (out.size() < n && std::chrono::steady_clock::now() < deadline) {
        const std::size_t got = src.read(buf.data(), std::min(buf.size(), n - out.size()));
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

// The command log from index `from`, compared whole. Prints what was seen on a
// mismatch, because "the third command was 0E 00 00 00 01, not 03 ..." is the
// whole finding.
bool commandsEqual(const std::vector<Bytes>& got, std::size_t from, const std::vector<Bytes>& want,
                   const char* what) {
    bool ok = got.size() >= from + want.size() && got.size() == from + want.size();
    for (std::size_t i = 0; ok && i < want.size(); ++i) { ok = got[from + i] == want[i]; }
    if (!ok) {
        std::printf("     %s: expected %zu commands after %zu, got %zu in all\n", what,
                    want.size(), from, got.size());
        for (std::size_t i = from; i < got.size(); ++i) {
            std::printf("       saw      %s\n", hex(got[i]).c_str());
        }
        for (const Bytes& w : want) { std::printf("       expected %s\n", hex(w).c_str()); }
    }
    return ok;
}

// What the open leaves on the wire for an R820T: rate, frequency, correction,
// digital AGC off, manual gain, the 29.7 dB rung, bias tee off. LITERALS.
std::vector<Bytes> openCommandsR820t() {
    return {
        {0x02, 0x00, 0x1F, 0x40, 0x00},  // sample rate 2048000
        {0x01, 0x05, 0xF5, 0xE1, 0x00},  // centre frequency 100000000
        {0x05, 0x00, 0x00, 0x00, 0x00},  // correction 0 ppm
        {0x08, 0x00, 0x00, 0x00, 0x00},  // digital AGC off
        {0x03, 0x00, 0x00, 0x00, 0x01},  // manual gain
        {0x04, 0x00, 0x00, 0x01, 0x29},  // tuner gain 297 tenths = 29.7 dB
        {0x0e, 0x00, 0x00, 0x00, 0x00},  // bias tee off
    };
}

// --- a scripted transport --------------------------------------------------
//
// What arrives, and when, chosen by the test rather than by the kernel. Used
// where a real socket cannot be trusted to do the thing being tested: TCP may
// coalesce the fake server's one-byte sends into one receive (so a carry test
// on the fake can pass with the carry deleted), and a real socket always wakes
// on shutdown() (so an abandoned reader cannot be made to happen).
//
// recvSome() delivers ONE queued chunk per call (split only when the caller's
// buffer is smaller), so the sizes the driver sees are exactly the sizes
// queued, and oddReceives counts the calls that handed it half a pair.
// shutdown() wakes a parked recvSome - unless ignoreShutdown, the stranded
// reader: that one returns only when the test sets `release`.
struct ScriptState {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Bytes> chunks;
    bool shut = false;
    bool release = false;
    bool ignoreShutdown = false;
    std::vector<std::uint8_t> sent;  // every byte the driver sent, in order
    std::atomic<std::size_t> consumedBytes{0};
    // recvSome() calls ENTERED. The driver's first receive is the header (one
    // call, made by open() itself); the reader's calls follow, so "the reader
    // has finished with chunk N and is parked for the next" is
    // recvEntered >= N + 2.
    std::atomic<int> recvEntered{0};
    std::atomic<int> oddReceives{0};
    std::atomic<int> shutdownCalls{0};
    std::atomic<int> closeCalls{0};

    void push(const Bytes& b) {
        {
            std::lock_guard<std::mutex> lk(m);
            chunks.push_back(b);
        }
        cv.notify_all();
    }
    void releaseStranded() {
        {
            std::lock_guard<std::mutex> lk(m);
            release = true;
        }
        cv.notify_all();
    }
    std::vector<Bytes> commands() {
        std::lock_guard<std::mutex> lk(m);
        std::vector<Bytes> out;
        for (std::size_t i = 0; i + 5 <= sent.size(); i += 5) {
            out.emplace_back(sent.begin() + static_cast<std::ptrdiff_t>(i),
                             sent.begin() + static_cast<std::ptrdiff_t>(i + 5));
        }
        return out;
    }
};

class ScriptedTransport : public cascade::source::iiod::Transport {
public:
    explicit ScriptedTransport(std::shared_ptr<ScriptState> st) : st_(std::move(st)) {}

    bool sendAll(const void* data, std::size_t n) override {
        std::lock_guard<std::mutex> lk(st_->m);
        const auto* p = static_cast<const std::uint8_t*>(data);
        st_->sent.insert(st_->sent.end(), p, p + n);
        return true;
    }
    bool recvAll(void* data, std::size_t n) override {
        auto* p = static_cast<std::uint8_t*>(data);
        std::size_t got = 0;
        while (got < n) {
            std::size_t part = 0;
            if (!recvSome(p + got, n - got, part)) { return false; }
            got += part;
        }
        return true;
    }
    bool recvSome(void* data, std::size_t max, std::size_t& got) override {
        got = 0;
        st_->recvEntered.fetch_add(1);
        std::unique_lock<std::mutex> lk(st_->m);
        if (st_->ignoreShutdown) {
            // THE STRANDED READER: parked until the test lets it go.
            st_->cv.wait(lk, [this] { return st_->release || !st_->chunks.empty(); });
            if (st_->chunks.empty()) {
                err_ = "scripted: released";
                return false;
            }
        } else {
            st_->cv.wait(lk, [this] { return !st_->chunks.empty() || st_->shut; });
            if (st_->chunks.empty()) {
                err_ = "the rtl_tcp server closed the connection";
                return false;
            }
        }
        Bytes& c = st_->chunks.front();
        const std::size_t n = std::min(max, c.size());
        std::memcpy(data, c.data(), n);
        c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(n));
        if (c.empty()) { st_->chunks.pop_front(); }
        got = n;
        st_->consumedBytes.fetch_add(n);
        if ((n & 1u) != 0) { st_->oddReceives.fetch_add(1); }
        return true;
    }
    void shutdown() override {
        st_->shutdownCalls.fetch_add(1);
        if (st_->ignoreShutdown) { return; }
        {
            std::lock_guard<std::mutex> lk(st_->m);
            st_->shut = true;
        }
        st_->cv.notify_all();
    }
    void close() override { st_->closeCalls.fetch_add(1); }
    const char* lastError() const override { return err_.c_str(); }

private:
    std::shared_ptr<ScriptState> st_;
    std::string err_;
};

// The RTL0 header an R820T with 29 gain steps sends.
Bytes scriptedHeader() {
    return {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};
}

// A script that has already "sent" its header, ready to open on.
std::shared_ptr<ScriptState> newScript(bool ignoreShutdown = false) {
    auto st = std::make_shared<ScriptState>();
    st->ignoreShutdown = ignoreShutdown;
    st->chunks.push_back(scriptedHeader());
    return st;
}

// `n` bytes all of one value: a chunk whose samples are all (v, v), so which
// chunk a delivered sample came from is readable off the sample itself.
Bytes tagged(std::uint8_t v, std::size_t n) { return Bytes(n, v); }

float tagFloat(std::uint8_t v) { return (static_cast<float>(v) - 127.5f) / 128.0f; }

bool hasWord(const std::string& s, const std::string& w) {
    return s.find(w) != std::string::npos;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. THE ADDRESS, parsed. The args are what the Open key and the saved
    //    config carry; a wrong reading of them opens the wrong server.
    // =====================================================================
    {
        std::string host;
        std::uint16_t port = 0;
        std::string error;
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp=192.168.1.20:1235", host, port, error));
        CHECK(host == "192.168.1.20" && port == 1235);
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp=sdr.example.org", host, port, error));
        CHECK(host == "sdr.example.org" && port == 1234);
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp=[fe80::1]:4321", host, port, error));
        CHECK(host == "fe80::1" && port == 4321);
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp=[::1]", host, port, error));
        CHECK(host == "::1" && port == 1234);
        // A bare IPv6 literal has no room for a port, so it is all host.
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp=fe80::2", host, port, error));
        CHECK(host == "fe80::2" && port == 1234);
        // Nothing typed: the server's own default place.
        CHECK(cascade::source::parseRtlTcpAddress("", host, port, error));
        CHECK(host == "127.0.0.1" && port == 1234);
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp=", host, port, error));
        CHECK(host == "127.0.0.1" && port == 1234);
        CHECK(cascade::source::parseRtlTcpAddress("rtltcp= 10.0.0.9 : 99 ", host, port, error) ==
              false);  // an inner space is not a port number
        // Refusals, each with a sentence.
        for (const char* bad : {"rtltcp=host:0", "rtltcp=host:65536", "rtltcp=host:abc",
                                "rtltcp=host:", "rtltcp=[::1", "rtltcp=[::1]x"}) {
            host.clear();
            const bool ok = cascade::source::parseRtlTcpAddress(bad, host, port, error);
            if (ok) { std::printf("     %s was accepted\n", bad); }
            CHECK(!ok);
            CHECK(!error.empty());
        }
        // The driver key and the tuner names.
        CHECK(std::string(RtlTcpSource().driverKey()) == "rtltcp");
        CHECK(std::string(cascade::source::rtlTcpTunerName(5)) == "R820T");
        CHECK(std::string(cascade::source::rtlTcpTunerName(6)) == "R828D");
        CHECK(std::string(cascade::source::rtlTcpTunerName(1)) == "E4000");
        CHECK(std::string(cascade::source::rtlTcpTunerName(0)) == "unknown tuner");
        CHECK(std::string(cascade::source::rtlTcpTunerName(99)) == "unknown tuner");
    }

    // =====================================================================
    // 2. OPEN parses the header; the name carries the tuner; gains() has the
    //    shape the tuner dictates; and the open leaves EXACTLY this on the
    //    wire. R820T first.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.tunerType.store(5);
        daemon.gainCount.store(29);
        daemon.streamEnabled.store(false);

        RtlTcpSource src;
        CHECK(!src.isOpen());
        CHECK(std::string(src.name()).find("not connected") != std::string::npos);
        CHECK(src.open(daemon.args()));
        CHECK(src.isOpen());
        CHECK(!src.running());
        CHECK(src.selfPaced());
        CHECK(src.tunerType() == 5);
        CHECK(src.tunerGainCount() == 29);
        CHECK(src.tunerName() == "R820T");
        // THE NAME CARRIES NO ADDRESS: it reaches the diagnostic log (the GUI
        // quotes it in "the %s refused a tune"), and PRIVACY.md says the host
        // is nowhere in the log. The tuner is in it; neither the host nor the
        // port is.
        const std::string name = src.name();
        CHECK(name == "rtl_tcp server: R820T (network)");
        CHECK(hasWord(name, "R820T"));
        CHECK(hasWord(name, "rtl_tcp"));
        CHECK(!hasWord(name, "127.0.0.1"));
        CHECK(!hasWord(name, std::to_string(daemon.port())));

        CHECK(waitForCommands(daemon, 0, 7));
        CHECK(commandsEqual(daemon.commands(0), 0, openCommandsR820t(), "R820T open"));

        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 1);
        if (g.size() == 1) {
            CHECK(g[0].name == "TUNER");
            CHECK(g[0].unit == GainUnit::Decibels);
            CHECK_NEAR(g[0].minDb, 0.0, 1e-9);
            CHECK_NEAR(g[0].maxDb, 49.6, 1e-9);
        }
        // manual gain at a middling level, as adoptDeviceMirrors expects
        CHECK(!src.autoGain());
        CHECK_NEAR(src.gainDb("TUNER"), 29.7, 1e-9);
        CHECK(src.autoGainSupported());
        CHECK(src.antennas().empty());
        CHECK(!src.setAntenna("x"));
        CHECK(src.hasFrequencyCorrection());
        CHECK_NEAR(src.sampleRateHz(), 2048000.0, 1e-9);
        CHECK_NEAR(src.centerFrequencyHz(), 100e6, 1e-9);
        double lo = 0.0;
        double hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 24e6, 1.0);
        CHECK_NEAR(hi, 1766e6, 1.0);
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(!rates.empty());
        CHECK(std::is_sorted(rates.begin(), rates.end()));
        CHECK(std::find(rates.begin(), rates.end(), 2048000.0) != rates.end());
        CHECK(std::find(rates.begin(), rates.end(), 2400000.0) != rates.end());

        // A second open on a live object is refused, not stacked.
        CHECK(!src.open(daemon.args()));

        src.closeDevice();
        CHECK(!src.isOpen());
        CHECK(waitFor([&] { return daemon.clientClosed() == 1; }, std::chrono::milliseconds(2000)));
    }

    // R828D is a decibel tuner too; E4000 and an unknown tuner are not.
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.streamEnabled.store(false);
        daemon.tunerType.store(6);
        daemon.gainCount.store(29);
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.tunerName() == "R828D");
        const auto g = src.gains();
        CHECK(g.size() == 1 && g[0].unit == GainUnit::Decibels);
        src.closeDevice();
    }
    {
        // E4000: STEPS, 0..13, the middle index at open (7), sent with 0x0d.
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.streamEnabled.store(false);
        daemon.tunerType.store(1);
        daemon.gainCount.store(14);
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(hasWord(src.name(), "E4000"));
        const auto g = src.gains();
        CHECK(g.size() == 1);
        if (g.size() == 1) {
            CHECK(g[0].unit == GainUnit::Steps);
            CHECK_NEAR(g[0].minDb, 0.0, 1e-9);
            CHECK_NEAR(g[0].maxDb, 13.0, 1e-9);
            CHECK_NEAR(g[0].stepDb, 1.0, 1e-9);
        }
        double lo = 0.0;
        double hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 52e6, 1.0);
        CHECK_NEAR(hi, 2200e6, 1.0);
        CHECK(waitForCommands(daemon, 0, 7));
        const std::vector<Bytes> want = {
            {0x02, 0x00, 0x1F, 0x40, 0x00}, {0x01, 0x05, 0xF5, 0xE1, 0x00},
            {0x05, 0x00, 0x00, 0x00, 0x00}, {0x08, 0x00, 0x00, 0x00, 0x00},
            {0x03, 0x00, 0x00, 0x00, 0x01}, {0x0d, 0x00, 0x00, 0x00, 0x07},
            {0x0e, 0x00, 0x00, 0x00, 0x00},
        };
        CHECK(commandsEqual(daemon.commands(0), 0, want, "E4000 open"));
        CHECK_NEAR(src.gainDb("TUNER"), 7.0, 1e-9);

        // Set by index: in range, then clamped both ways.
        CHECK(src.setGainDb("TUNER", 5.0));
        CHECK_NEAR(src.gainDb("TUNER"), 5.0, 1e-9);
        CHECK(src.setGainDb("TUNER", 100.0));
        CHECK_NEAR(src.gainDb("TUNER"), 13.0, 1e-9);
        CHECK(src.setGainDb("TUNER", -4.0));
        CHECK_NEAR(src.gainDb("TUNER"), 0.0, 1e-9);
        CHECK(waitForCommands(daemon, 0, 10));
        CHECK(commandsEqual(daemon.commands(0), 7,
                            {{0x0d, 0x00, 0x00, 0x00, 0x05},
                             {0x0d, 0x00, 0x00, 0x00, 0x0d},
                             {0x0d, 0x00, 0x00, 0x00, 0x00}},
                            "E4000 gain by index"));
        src.closeDevice();
    }
    {
        // A tuner the header does not name, with no gain steps: 0..0, nothing
        // sent for a gain, the default range.
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.streamEnabled.store(false);
        daemon.tunerType.store(0);
        daemon.gainCount.store(0);
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(hasWord(src.name(), "unknown tuner"));
        const auto g = src.gains();
        CHECK(g.size() == 1);
        if (g.size() == 1) {
            CHECK(g[0].unit == GainUnit::Steps);
            CHECK_NEAR(g[0].minDb, 0.0, 1e-9);
            CHECK_NEAR(g[0].maxDb, 0.0, 1e-9);
        }
        double lo = 0.0;
        double hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 24e6, 1.0);
        CHECK_NEAR(hi, 1766e6, 1.0);
        CHECK(waitForCommands(daemon, 0, 6));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(daemon.commands(0).size() == 6);  // no gain command at all
        CHECK(src.setGainDb("TUNER", 12.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(daemon.commands(0).size() == 6);
        src.closeDevice();
    }

    // =====================================================================
    // 3. THE EXACT BYTES each setter sends: id, then a big-endian u32.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.streamEnabled.store(false);
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(waitForCommands(daemon, 0, 7));
        std::size_t n = 7;
        const auto expect = [&](const std::vector<Bytes>& want, const char* what) {
            CHECK(waitForCommands(daemon, 0, n + want.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK(commandsEqual(daemon.commands(0), n, want, what));
            n += want.size();
        };

        // Centre frequency: 433.92 MHz = 0x19DD1800.
        CHECK(src.setCenterFrequencyHz(433.92e6));
        CHECK_NEAR(src.centerFrequencyHz(), 433.92e6, 1e-9);
        expect({{0x01, 0x19, 0xDD, 0x18, 0x00}}, "setCenterFrequencyHz");

        // Sample rate: 2.4 MS/s is on the menu (0x00249F00)...
        CHECK(src.setSampleRateHz(2.4e6));
        CHECK_NEAR(src.sampleRateHz(), 2.4e6, 1e-9);
        expect({{0x02, 0x00, 0x24, 0x9F, 0x00}}, "setSampleRateHz 2.4e6");
        // ...2.5 MS/s is not, and snaps to the nearest menu entry, 2.56 (0x00271000).
        CHECK(src.setSampleRateHz(2.5e6));
        CHECK_NEAR(src.sampleRateHz(), 2.56e6, 1e-9);
        expect({{0x02, 0x00, 0x27, 0x10, 0x00}}, "setSampleRateHz snapped");
        CHECK(!src.setSampleRateHz(0.0));
        CHECK(!src.setSampleRateHz(-5.0));

        // Gain: 10.0 dB snaps to the 8.7 dB rung (87 tenths), not 12.5.
        CHECK(src.setGainDb("TUNER", 10.0));
        CHECK_NEAR(src.gainDb("TUNER"), 8.7, 1e-9);
        expect({{0x04, 0x00, 0x00, 0x00, 0x57}}, "setGainDb nearest rung");
        CHECK(src.setGainDb("tuner", 29.9));  // the name is not case-sensitive
        CHECK_NEAR(src.gainDb("TUNER"), 29.7, 1e-9);
        expect({{0x04, 0x00, 0x00, 0x01, 0x29}}, "setGainDb 29.7");
        CHECK(!src.setGainDb("LNA", 10.0));
        CHECK(hasWord(src.lastError(), "LNA"));

        // Automatic gain: gain mode 0; and off again re-sends the manual level.
        CHECK(src.setAutoGain(true));
        CHECK(src.autoGain());
        expect({{0x03, 0x00, 0x00, 0x00, 0x00}}, "setAutoGain(true)");
        CHECK(src.setAutoGain(false));
        CHECK(!src.autoGain());
        expect({{0x03, 0x00, 0x00, 0x00, 0x01}, {0x04, 0x00, 0x00, 0x01, 0x29}},
               "setAutoGain(false)");
        // A hand-set gain while automatic goes to manual first.
        CHECK(src.setAutoGain(true));
        expect({{0x03, 0x00, 0x00, 0x00, 0x00}}, "setAutoGain(true) again");
        CHECK(src.setGainDb("TUNER", 10.0));
        CHECK(!src.autoGain());
        expect({{0x03, 0x00, 0x00, 0x00, 0x01}, {0x04, 0x00, 0x00, 0x00, 0x57}},
               "setGainDb while automatic");

        // Frequency correction -3 ppm is the two's complement 0xFFFFFFFD, and
        // is followed by the centre frequency again.
        CHECK(src.setFrequencyCorrectionPpm(-3.0));
        expect({{0x05, 0xFF, 0xFF, 0xFF, 0xFD}, {0x01, 0x19, 0xDD, 0x18, 0x00}},
               "setFrequencyCorrectionPpm(-3)");
        // Whole ppm: 1.6 rounds to 2.
        CHECK(src.setFrequencyCorrectionPpm(1.6));
        expect({{0x05, 0x00, 0x00, 0x00, 0x02}, {0x01, 0x19, 0xDD, 0x18, 0x00}},
               "setFrequencyCorrectionPpm(1.6)");

        // Bias tee.
        CHECK(!src.biasT());
        CHECK(src.setBiasT(true));
        CHECK(src.biasT());
        expect({{0x0e, 0x00, 0x00, 0x00, 0x01}}, "setBiasT(true)");
        // ...and the GUI's dynamic_cast chain reaches it.
        bool reached = false;
        CHECK(cascade::gui::withBiasTee(&src, [&reached](auto& r) {
            reached = true;
            return r.setBiasT(false);
        }));
        CHECK(reached);
        CHECK(!src.biasT());
        expect({{0x0e, 0x00, 0x00, 0x00, 0x00}}, "setBiasT(false)");

        // A frequency the 32-bit field cannot carry is refused, and nothing is sent.
        CHECK(!src.setCenterFrequencyHz(5e9));
        CHECK(!src.setCenterFrequencyHz(0.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(daemon.commands(0).size() == n);
        src.closeDevice();
    }

    // =====================================================================
    // 4. THE LAUNCH ORDER: open(), then straight away a rate (what
    //    launchDeviceOpen does), then the stream - which must start.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.setSampleRateHz(2.0e6));  // nearest menu entry to 2 MS/s
        CHECK_NEAR(src.sampleRateHz(), 2048000.0, 1e-9);
        CHECK(src.setAutoGain(false));      // adoptDeviceMirrors
        CHECK(src.start());
        CHECK(src.running());
        const auto got = readN(src, 2000, std::chrono::milliseconds(3000));
        CHECK(got.size() == 2000);
        // The sample-rate command after open() reached the server.
        CHECK(waitForCommands(daemon, 0, 9));
        const std::vector<Bytes> cmds = daemon.commands(0);
        CHECK(cmds.size() >= 9);
        if (cmds.size() >= 9) {
            CHECK(cmds[7] == (Bytes{0x02, 0x00, 0x1F, 0x40, 0x00}));
            CHECK(cmds[8] == (Bytes{0x03, 0x00, 0x00, 0x00, 0x01}));
        }
        CHECK(!src.faulted());
        // The stream-health line is the one the other drivers write.
        const std::string line = src.streamHealthLine();
        CHECK(line.rfind("source: stream health - reads ", 0) == 0);
        src.closeDevice();
    }

    // =====================================================================
    // 5. THE STREAM DECODES, end to end on a real socket (the DETERMINISTIC
    //    odd-byte test, on a scripted transport that cannot be coalesced, is
    //    section 10: the kernel may merge this fake's one-byte sends). Byte n
    //    has the value n & 0xFF, so sample k is
    //    (byte 2k, byte 2k+1); the server first sends 1 byte at a time, so the
    //    reader is handed half a pair on every other receive, then odd-sized
    //    chunks. An odd trailing byte that was dropped, or treated as an I, would
    //    swap I and Q for the rest of the stream - and shows here as a mismatch.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.streamEnabled.store(false);
        daemon.trickleFirst.store(true);
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.start());
        daemon.streamEnabled.store(true);  // only now, with the reader already waiting
        const std::size_t want = 6000;
        const auto got = readN(src, want, std::chrono::milliseconds(8000));
        CHECK(got.size() == want);
        std::size_t bad = 0;
        std::size_t firstBad = 0;
        for (std::size_t k = 0; k < got.size(); ++k) {
            const float i = (static_cast<float>((2 * k) & 0xFF) - 127.5f) / 128.0f;
            const float q = (static_cast<float>((2 * k + 1) & 0xFF) - 127.5f) / 128.0f;
            if (std::fabs(got[k].real() - i) > 1e-6f || std::fabs(got[k].imag() - q) > 1e-6f) {
                if (bad == 0) { firstBad = k; }
                ++bad;
            }
        }
        if (bad != 0) {
            std::printf("     %zu of %zu samples decoded wrongly, the first at %zu\n", bad,
                        got.size(), firstBad);
        }
        CHECK(bad == 0);
        // Literals, independent of the driver's table: byte 0 is -127.5/128,
        // byte 1 is -126.5/128; byte 127 and 128 straddle zero; 255 is just
        // inside +1.
        if (got.size() >= 128) {
            CHECK_NEAR(got[0].real(), -0.99609375, 1e-7);
            CHECK_NEAR(got[0].imag(), -0.98828125, 1e-7);
            CHECK_NEAR(got[63].imag(), -0.00390625, 1e-7);  // byte 127
            CHECK_NEAR(got[64].real(), 0.00390625, 1e-7);   // byte 128
            CHECK_NEAR(got[127].imag(), 0.99609375, 1e-7);  // byte 255
        }
        CHECK(!src.faulted());
        CHECK(src.droppedBuffers() == 0);
        src.closeDevice();
    }

    // =====================================================================
    // 6. THE SERVER GOES AWAY mid-stream: the fault latches, the reason names
    //    the connection, the device is dead, read() returns nothing, and
    //    nothing further is sent to it.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.dieAfterBytes.store(4000);
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(5000)));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        const std::string err = src.lastError();
        if (err.empty() || !hasWord(err, "connection")) {
            std::printf("     lastError was \"%s\"\n", err.c_str());
        }
        CHECK(!err.empty());
        CHECK(hasWord(err, "connection to the rtl_tcp server was lost while reading samples"));
        CHECK(hasWord(err, "closed the connection"));
        // The 4000 bytes sent before the hang-up (2000 samples) are still
        // delivered; after that read() is a zero, over and over.
        std::vector<std::complex<float>> buf(8192);
        std::size_t total = 0;
        for (int i = 0; i < 20; ++i) { total += src.read(buf.data(), buf.size()); }
        CHECK(total == 2000);
        CHECK(src.read(buf.data(), buf.size()) == 0);
        CHECK(src.read(buf.data(), buf.size()) == 0);
        // A dead connection refuses everything and keeps its FIRST cause.
        CHECK(!src.setCenterFrequencyHz(100e6));
        CHECK(!src.setGainDb("TUNER", 10.0));
        CHECK(!src.start());
        CHECK(std::string(src.lastError()) == err);
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 7. OPEN FAILURES, worded so classifyRadioOpen files them right.
    // =====================================================================
    {
        // Nothing listening: bind a port, learn it, close it.
        FakeRtlTcp holder;
        CHECK(holder.start());
        const std::string args = holder.args();
        holder.stop();
        RtlTcpSource src;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!src.open(args));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        const std::string err = src.lastError();
        std::printf("     refused port: %lld ms, \"%s\"\n", static_cast<long long>(ms),
                    err.c_str());
        CHECK(!src.isOpen());
        CHECK(hasWord(err, "rtl_tcp"));
        CHECK(!hasWord(err, "Pluto"));
        // The anonymous count says "no server there" for a refusal, and "did
        // not answer" only for a connect that timed out.
        const health::RadioReason why = health::classifyRadioOpen("rtltcp", err);
        CHECK(why == health::RadioReason::Absent || why == health::RadioReason::Timeout);
        if (!hasWord(err, "did not answer")) { CHECK(why == health::RadioReason::Absent); }
        CHECK(!src.start());
        CHECK(!src.setCenterFrequencyHz(100e6));  // nothing is open
    }
    {
        // Something that answers, but not with an rtl_tcp header.
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.header.store(FakeRtlTcp::Header::BadMagic);
        RtlTcpSource src;
        CHECK(!src.open(daemon.args()));
        const std::string err = src.lastError();
        CHECK(hasWord(err, "nothing at 127.0.0.1:" + std::to_string(daemon.port())));
        CHECK(hasWord(err, "speaks rtl_tcp"));
        CHECK(health::classifyRadioOpen("rtltcp", err) == health::RadioReason::Absent);
        CHECK(!src.isOpen());
        // ...and a failed open leaves the object usable for another try.
        daemon.header.store(FakeRtlTcp::Header::Normal);
        daemon.streamEnabled.store(false);
        CHECK(src.open(daemon.args()));
        CHECK(src.isOpen());
        CHECK(std::string(src.lastError()).empty());
        src.closeDevice();
    }
    {
        // A port that accepts and hangs up.
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.header.store(FakeRtlTcp::Header::CloseImmediately);
        RtlTcpSource src;
        CHECK(!src.open(daemon.args()));
        const std::string err = src.lastError();
        CHECK(hasWord(err, "nothing at"));
        CHECK(hasWord(err, "closed the connection"));
        CHECK(health::classifyRadioOpen("rtltcp", err) == health::RadioReason::Absent);
    }
    {
        // A port that accepts and says nothing: the header read times out, and
        // the sentence says so (the anonymous count files it as a timeout).
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.header.store(FakeRtlTcp::Header::Silent);
        RtlTcpSource src;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!src.open(daemon.args()));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        const std::string err = src.lastError();
        CHECK(hasWord(err, "timed out"));
        CHECK(health::classifyRadioOpen("rtltcp", err) == health::RadioReason::Timeout);
        CHECK(ms >= 1500 && ms < 6000);  // the 2000 ms receive bound, measured
    }
    {
        // An address that is not one.
        RtlTcpSource src;
        CHECK(!src.open("rtltcp=host:99999"));
        CHECK(hasWord(src.lastError(), "99999"));
        CHECK(!src.isOpen());
        // An unresolvable name.
        RtlTcpSource src2;
        CHECK(!src2.open("rtltcp=no-such-host.invalid:1234"));
        const std::string err = src2.lastError();
        std::printf("     unknown host: \"%s\"\n", err.c_str());
        CHECK(hasWord(err, "rtl_tcp"));
        CHECK(!hasWord(err, "Pluto"));
        CHECK(health::classifyRadioOpen("rtltcp", err) == health::RadioReason::Absent ||
              health::classifyRadioOpen("rtltcp", err) == health::RadioReason::Timeout);
    }

    // =====================================================================
    // 8. STOP PAUSES, IT DOES NOT RECONNECT. The connection and the reader live
    //    from open() to closeDevice(): stop() keeps the socket (so the server
    //    stays busy until the source is CLOSED, which is the documented price),
    //    a setter made while stopped is sent AT ONCE (the server's state never
    //    changed, so start() has nothing to replay), and start() makes no
    //    connection and sends nothing.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.setCenterFrequencyHz(145.5e6));
        CHECK(src.setSampleRateHz(2.4e6));
        CHECK(src.setFrequencyCorrectionPpm(-3.0));
        CHECK(src.setBiasT(true));
        CHECK(src.setGainDb("TUNER", 10.0));
        CHECK(src.start());
        CHECK(readN(src, 500, std::chrono::milliseconds(3000)).size() == 500);
        src.stop();
        CHECK(!src.running());
        // The socket is NOT dropped by a stop: the server's accept count and its
        // view of the client are unchanged however long we wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(daemon.clientClosed() == 0);
        CHECK(daemon.connections() == 1);
        CHECK(!src.faulted());
        CHECK(src.isOpen());
        // Stopped: a change goes to the server at once.
        const std::size_t before = daemon.commands(0).size();
        CHECK(src.setCenterFrequencyHz(96.3e6));
        CHECK_NEAR(src.centerFrequencyHz(), 96.3e6, 1e-9);
        CHECK(waitForCommands(daemon, 0, before + 1));
        CHECK(commandsEqual(daemon.commands(0), before, {{0x01, 0x05, 0xBD, 0x6B, 0xE0}},
                            "a tune made while stopped"));
        // Start: no new connection, nothing replayed, and the stream flows.
        CHECK(src.start());
        CHECK(src.running());
        CHECK(daemon.connections() == 1);
        CHECK(readN(src, 500, std::chrono::milliseconds(3000)).size() == 500);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(daemon.commands(0).size() == before + 1);
        CHECK(daemon.connections() == 1);
        CHECK(daemon.clientClosed() == 0);
        CHECK(!src.faulted());
        // Stop and start again, several times: still one connection.
        for (int i = 0; i < 5; ++i) {
            src.stop();
            CHECK(src.start());
        }
        CHECK(readN(src, 200, std::chrono::milliseconds(3000)).size() == 200);
        CHECK(daemon.connections() == 1);
        // CLOSING is what frees the server.
        src.closeDevice();
        CHECK(waitFor([&] { return daemon.clientClosed() == 1; }, std::chrono::milliseconds(3000)));
    }

    // =====================================================================
    // 9. A SERVER THAT STALLS. stop() is a flag flip: it returns at once, in
    //    well under 100 ms, against a server that has gone silent, makes no
    //    network call and abandons nothing; closeDevice() wakes the parked
    //    reader with shutdown() instead of waiting out the socket's 2 s bound.
    //    A server that goes quiet mid-stream faults the source on that bound.
    // =====================================================================
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.stallAfterBytes.store(0);  // header, then nothing at all
        const unsigned long long abandonedBefore = RtlTcpSource::readersAbandoned();
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        std::printf("     stop() against a silent server took %lld ms\n",
                    static_cast<long long>(ms));
        CHECK(ms < 100);
        CHECK(!src.running());
        CHECK(!src.faulted());
        // start() is as quick, and refuses nothing: no connection to make.
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(src.start());
        const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t1)
                             .count();
        CHECK(ms2 < 100);
        src.stop();
        CHECK(daemon.connections() == 1);
        // closeDevice: the reader is parked in a receive on a silent server and
        // is woken by shutdown(), so the close takes milliseconds, not the 2 s
        // receive bound.
        const auto t2 = std::chrono::steady_clock::now();
        src.closeDevice();
        const auto ms3 = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t2)
                             .count();
        std::printf("     closeDevice() against a silent server took %lld ms\n",
                    static_cast<long long>(ms3));
        CHECK(ms3 < 500);
        CHECK(RtlTcpSource::readersAbandoned() == abandonedBefore);
        CHECK(waitFor([&] { return daemon.clientClosed() == 1; }, std::chrono::milliseconds(2000)));
    }
    {
        FakeRtlTcp daemon;
        CHECK(daemon.start());
        daemon.stallAfterBytes.store(2000);  // 1000 samples, then silence
        RtlTcpSource src;
        CHECK(src.open(daemon.args()));
        CHECK(src.start());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(7000)));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms >= 1500);  // the lower edge: nothing faulted instantly
        CHECK(src.deviceDead());
        CHECK(hasWord(src.lastError(), "timed out"));
        CHECK(hasWord(src.lastError(), "lost while reading samples"));
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 10. ON A SCRIPTED TRANSPORT (exact chunk sizes, no kernel to coalesce
    //     them): the odd-byte carry, what a stop and a start deliver, an idle
    //     read, and a reader that never comes back.
    // =====================================================================
    {
        // THE CARRY, DETERMINISTICALLY. Chunks of odd and even sizes, so half
        // a pair ends many receives; byte n of the stream is n & 0xFF, so a
        // dropped byte or an I/Q swap is a mismatch at a named sample. The
        // driver is handed exactly these sizes: oddReceives proves it (a
        // coalescing kernel could not promise that).
        auto script = newScript();
        RtlTcpSource src;
        CHECK(src.openWithTransportForTest("rtltcp=scripted.invalid:1234",
                                           std::make_unique<ScriptedTransport>(script)));
        CHECK(src.start());
        const std::size_t sizes[] = {1, 3, 5, 7, 2, 9, 1, 1, 4, 11, 13, 1, 6, 255, 3};
        std::size_t total = 0;
        int oddChunks = 0;
        for (const std::size_t n : sizes) {
            Bytes chunk(n);
            for (std::size_t i = 0; i < n; ++i) {
                chunk[i] = static_cast<std::uint8_t>((total + i) & 0xFF);
            }
            script->push(chunk);
            total += n;
            if ((n & 1u) != 0) { ++oddChunks; }
        }
        // An even total, so every byte pairs up in the end.
        if ((total & 1u) != 0) {
            script->push(Bytes{static_cast<std::uint8_t>(total & 0xFF)});
            ++total;
            ++oddChunks;
        }
        const std::size_t want = total / 2;
        const auto got = readN(src, want, std::chrono::milliseconds(5000));
        CHECK(got.size() == want);
        CHECK(script->oddReceives.load() == oddChunks);
        CHECK(script->oddReceives.load() > 0);
        std::size_t bad = 0;
        for (std::size_t k = 0; k < got.size(); ++k) {
            const float i = tagFloat(static_cast<std::uint8_t>((2 * k) & 0xFF));
            const float q = tagFloat(static_cast<std::uint8_t>((2 * k + 1) & 0xFF));
            if (std::fabs(got[k].real() - i) > 1e-6f || std::fabs(got[k].imag() - q) > 1e-6f) {
                ++bad;
            }
        }
        if (bad != 0) { std::printf("     %zu of %zu samples decoded wrongly\n", bad, got.size()); }
        CHECK(bad == 0);
        CHECK(!src.faulted());
        src.closeDevice();
    }
    {
        // WHAT A STOP AND A START DELIVER. Chunk A is partly read, then stop;
        // chunk B arrives WHILE STOPPED (and must be consumed - the server is
        // never back-pressured - and thrown away); then start; then chunk C.
        // Only C's samples may come out, and the socket is never touched by
        // the stop or the start.
        auto script = newScript();
        RtlTcpSource src;
        CHECK(src.openWithTransportForTest("rtltcp=scripted.invalid:1234",
                                           std::make_unique<ScriptedTransport>(script)));
        const std::size_t headerBytes = 12;
        CHECK(src.start());
        script->push(tagged(0x10, 200));  // chunk A: 100 samples
        // Calls entered: 1 the header, 2 the one that returned A, 3 parked for
        // the next - so the reader has put all of A in the ring.
        CHECK(waitFor([&] { return script->recvEntered.load() >= 3; },
                      std::chrono::milliseconds(3000)));
        std::vector<std::complex<float>> buf(64);
        const auto a = readN(src, 40, std::chrono::milliseconds(3000));
        CHECK(a.size() == 40);  // the other 60 of A stay in the ring
        for (const auto& s : a) { CHECK_NEAR(s.real(), tagFloat(0x10), 1e-6); }
        const std::size_t sentBefore = script->commands().size();
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms < 100);
        CHECK(!src.running());
        // B arrives while stopped: consumed (not left in the socket), kept nowhere.
        script->push(tagged(0x20, 200));
        CHECK(waitFor([&] { return script->recvEntered.load() >= 4; },
                      std::chrono::milliseconds(3000)));
        CHECK(script->consumedBytes.load() == headerBytes + 400);
        // A LONG STOP: 20 receives of 64 KiB, more than the ring (2^19 samples)
        // could ever hold. Discarded as they arrive, they drop nothing; kept,
        // the ring would overflow and the drop counter would say so.
        for (int i = 0; i < 20; ++i) { script->push(tagged(0x40, 65536)); }
        CHECK(waitFor([&] { return script->recvEntered.load() >= 24; },
                      std::chrono::milliseconds(5000)));
        CHECK(script->consumedBytes.load() == headerBytes + 400 + 20 * 65536);
        CHECK(src.droppedBuffers() == 0);
        CHECK(script->shutdownCalls.load() == 0);
        CHECK(script->closeCalls.load() == 0);
        CHECK(src.start());
        CHECK(script->shutdownCalls.load() == 0);
        CHECK(script->closeCalls.load() == 0);
        CHECK(script->commands().size() == sentBefore);  // nothing replayed
        // Nothing from before the stop comes out of the start.
        std::size_t stale = 0;
        for (int i = 0; i < 3; ++i) { stale += src.read(buf.data(), buf.size()); }
        CHECK(stale == 0);
        script->push(tagged(0x30, 100));  // chunk C: 50 samples
        const auto c = readN(src, 50, std::chrono::milliseconds(3000));
        CHECK(c.size() == 50);
        std::size_t wrong = 0;
        for (const auto& s : c) {
            if (std::fabs(s.real() - tagFloat(0x30)) > 1e-6f ||
                std::fabs(s.imag() - tagFloat(0x30)) > 1e-6f) {
                ++wrong;
            }
        }
        if (wrong != 0) { std::printf("     %zu samples were not from chunk C\n", wrong); }
        CHECK(wrong == 0);
        CHECK(src.read(buf.data(), buf.size()) == 0);  // and nothing else was kept
        CHECK(!src.faulted());
        src.closeDevice();
        CHECK(script->shutdownCalls.load() == 1);  // the close woke the reader...
        CHECK(script->closeCalls.load() == 1);     // ...and then closed the socket
    }
    {
        // AN IDLE read() waits for samples no longer than kReadWait (plus the
        // scheduler's slack) and then returns the contract's "nothing yet".
        auto script = newScript();
        RtlTcpSource src;
        CHECK(src.openWithTransportForTest("rtltcp=scripted.invalid:1234",
                                           std::make_unique<ScriptedTransport>(script)));
        CHECK(src.start());
        std::vector<std::complex<float>> buf(128);
        long long worst = 0;
        long long best = 1000000;
        for (int i = 0; i < 5; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(src.read(buf.data(), buf.size()) == 0);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            worst = std::max<long long>(worst, ms);
            best = std::min<long long>(best, ms);
        }
        std::printf("     idle read(): %lld..%lld ms (kReadWait %lld)\n", best, worst,
                    static_cast<long long>(RtlTcpSource::kReadWait.count()));
        CHECK(worst <= RtlTcpSource::kReadWait.count() + 40);
        CHECK(best >= RtlTcpSource::kReadWait.count() / 2);  // it did wait, not spin
        CHECK(!src.faulted());
        src.closeDevice();
    }
    {
        // A READER THAT NEVER COMES BACK: the script ignores shutdown(), so the
        // join runs out its bound. closeDevice() returns within kReaderJoinWait
        // (plus slack), the process-wide abandon counter goes up by one, the
        // source is condemned with the reason named, and the stranded thread -
        // released afterwards - finds its link alive and exits without touching
        // a destroyed object.
        auto script = newScript(true);
        const unsigned long long abandonedBefore = RtlTcpSource::readersAbandoned();
        {
            RtlTcpSource src;
            CHECK(src.openWithTransportForTest("rtltcp=scripted.invalid:1234",
                                               std::make_unique<ScriptedTransport>(script)));
            CHECK(src.start());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto t0 = std::chrono::steady_clock::now();
            src.closeDevice();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            std::printf("     closeDevice() with a stranded reader took %lld ms (bound %lld)\n",
                        static_cast<long long>(ms),
                        static_cast<long long>(RtlTcpSource::kReaderJoinWait.count()));
            CHECK(ms >= RtlTcpSource::kReaderJoinWait.count() - 100);
            CHECK(ms < RtlTcpSource::kReaderJoinWait.count() + 1000);
            CHECK(RtlTcpSource::readersAbandoned() == abandonedBefore + 1);
            CHECK(script->shutdownCalls.load() == 1);  // it was asked to wake first
            CHECK(script->closeCalls.load() == 0);     // and its socket was NOT closed under it
            CHECK(src.deviceDead());
            CHECK(src.faultedWhile() == "waiting for the sample reader to stop");
            CHECK(!src.isOpen());
            CHECK(!src.start());
            CHECK(!src.open("rtltcp=scripted.invalid:1234"));  // condemned: no reopen
        }  // the destructor runs closeDevice() again: nothing left to join
        script->releaseStranded();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(RtlTcpSource::readersAbandoned() == abandonedBefore + 1);
    }

    // =====================================================================
    // 11. PRIVACY: nothing the source wrote to the diagnostic log carries the
    //     host. (127.0.0.1 is the host every test above used.)
    // =====================================================================
    {
        bool sawOpen = false;
        bool sawFault = false;
        for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
            if (line.find("rtltcp:") == std::string::npos &&
                line.find("rtl_tcp") == std::string::npos) {
                continue;
            }
            if (line.find("127.0.0.1") != std::string::npos) {
                std::printf("     host in a log line: %s\n", line.c_str());
            }
            CHECK(line.find("127.0.0.1") == std::string::npos);
            if (line.find("rtltcp: opened") != std::string::npos) { sawOpen = true; }
            if (line.find("rtltcp: failed while reading samples") != std::string::npos) {
                sawFault = true;
            }
        }
        CHECK(sawOpen);
        CHECK(sawFault);
    }

    return testSummary("test_rtl_tcp_source");
}
