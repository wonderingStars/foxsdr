// net_sample_source.cpp - see net_sample_source.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/net_sample_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/health_events.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::source {

namespace {

std::atomic<unsigned long long> g_abandoned{0};

#if defined(_WIN32)
constexpr std::intptr_t kBadSock = static_cast<std::intptr_t>(INVALID_SOCKET);
bool ensureNet() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
void closeSock(std::intptr_t s) {
    if (s != kBadSock) { ::closesocket(static_cast<SOCKET>(s)); }
}
void shutdownSock(std::intptr_t s) { ::shutdown(static_cast<SOCKET>(s), SD_BOTH); }
int lastErr() { return ::WSAGetLastError(); }
using SockT = SOCKET;
#else
constexpr std::intptr_t kBadSock = -1;
bool ensureNet() { return true; }
void closeSock(std::intptr_t s) {
    if (s != kBadSock) { ::close(static_cast<int>(s)); }
}
void shutdownSock(std::intptr_t s) { ::shutdown(static_cast<int>(s), SHUT_RDWR); }
int lastErr() { return errno; }
using SockT = int;
#endif

// True when the socket becomes readable within `ms`. Select, never SO_RCVTIMEO: that does not
// bound anything it should on Windows datagram sockets the way a caller expects, and select
// is bounded the same way everywhere.
bool waitReadable(std::intptr_t s, long ms, bool& error) {
    error = false;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(static_cast<SockT>(s), &rf);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
#if defined(_WIN32)
    const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
    const int r = ::select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv);
#endif
    if (r < 0) { error = true; return false; }
    return r > 0;
}

}  // namespace

bool parseHostPort(const std::string& text, std::uint16_t defaultPort, std::string& host,
                   std::uint16_t& port, std::string& error) {
    host.clear();
    port = defaultPort;
    std::string rest = text;
    std::string portText;
    if (!rest.empty() && rest[0] == '[') {
        const std::size_t close = rest.find(']');
        if (close == std::string::npos) { error = "an address with an unclosed bracket"; return false; }
        host = rest.substr(1, close - 1);
        if (close + 1 < rest.size()) {
            if (rest[close + 1] != ':') { error = "text after the bracketed address"; return false; }
            portText = rest.substr(close + 2);
        }
    } else {
        const std::size_t colon = rest.find(':');
        if (colon != std::string::npos && rest.find(':', colon + 1) == std::string::npos) {
            host = rest.substr(0, colon);
            portText = rest.substr(colon + 1);
        } else {
            host = rest;  // a name, a dotted quad, or a bare IPv6 literal
        }
    }
    if (host.empty()) { error = "the server address is empty"; return false; }
    if (!portText.empty()) {
        char* end = nullptr;
        const long v = std::strtol(portText.c_str(), &end, 10);
        if (end == portText.c_str() || *end != '\0' || v < 1 || v > 65535) {
            error = "the port is not a number from 1 to 65535";
            return false;
        }
        port = static_cast<std::uint16_t>(v);
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// UdpTransport
// ---------------------------------------------------------------------------------------

UdpTransport::~UdpTransport() { close(); }

std::unique_ptr<UdpTransport> UdpTransport::bindLocal(const std::string& localHost,
                                                      std::uint16_t localPort,
                                                      std::chrono::milliseconds ioWait,
                                                      std::string& error) {
    if (!ensureNet()) { error = "the network could not be started"; return nullptr; }
    std::unique_ptr<UdpTransport> t(new UdpTransport());
    t->ioWait_ = ioWait;
    const SockT s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (static_cast<std::intptr_t>(s) == kBadSock) { error = "could not open a UDP socket"; return nullptr; }
    t->sock_ = static_cast<std::intptr_t>(s);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(localPort);
    if (localHost.empty() || localHost == "0.0.0.0") {
        a.sin_addr.s_addr = ::htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, localHost.c_str(), &a.sin_addr) != 1) {
        error = "the local address is not a dotted IPv4 address";
        return nullptr;
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        error = "could not bind a UDP socket";
        return nullptr;
    }
    sockaddr_in b{};
#if defined(_WIN32)
    int len = sizeof(b);
#else
    socklen_t len = sizeof(b);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&b), &len) != 0) {
        error = "could not read the UDP socket's port";
        return nullptr;
    }
    t->localPort_ = ::ntohs(b.sin_port);
    return t;
}

bool UdpTransport::connectTo(const std::string& host, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
            error_ = "could not find the host " + host;
            return false;
        }
        a.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        ::freeaddrinfo(res);
    }
    if (::connect(static_cast<SockT>(sock_), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        error_ = "could not set the UDP peer";
        return false;
    }
    return true;
}

bool UdpTransport::sendAll(const void* data, std::size_t n) {
    if (sock_ == kBadSock) { error_ = "the UDP socket is closed"; return false; }
    const int r = ::send(static_cast<SockT>(sock_), static_cast<const char*>(data),
#if defined(_WIN32)
                         static_cast<int>(n),
#else
                         n,
#endif
                         0);
    if (r < 0 || static_cast<std::size_t>(r) != n) {
        error_ = "a UDP send failed";
        return false;
    }
    return true;
}

bool UdpTransport::recvSome(void* data, std::size_t max, std::size_t& got) {
    got = 0;
    if (sock_ == kBadSock || shut_.load()) { error_ = "the UDP socket is closed"; return false; }
    bool err = false;
    if (!waitReadable(sock_, static_cast<long>(ioWait_.count()), err)) {
        error_ = err ? "the UDP socket failed" : "no datagram arrived (receive timed out)";
        return false;
    }
    if (shut_.load()) { error_ = "the UDP socket is closed"; return false; }
    const int r = ::recv(static_cast<SockT>(sock_), static_cast<char*>(data),
#if defined(_WIN32)
                         static_cast<int>(max),
#else
                         max,
#endif
                         0);
    if (r < 0) {
#if defined(_WIN32)
        // A datagram larger than the buffer is truncated to it (WSAEMSGSIZE) - the bytes that
        // fit are delivered, as on every other platform.
        if (lastErr() == WSAEMSGSIZE) { got = max; return true; }
#endif
        error_ = "a UDP receive failed";
        return false;
    }
    got = static_cast<std::size_t>(r);
    return true;
}

bool UdpTransport::recvAll(void* data, std::size_t n) {
    std::size_t got = 0;
    if (!recvSome(data, n, got)) { return false; }
    if (got != n) { error_ = "a datagram of the wrong size arrived"; return false; }
    return true;
}

void UdpTransport::shutdown() {
    shut_.store(true);
    // A datagram socket has no peer to hang up on: the parked receive notices shut_ at its next
    // select tick (ioWait_ at the most). A zero-length datagram to ourselves would wake it
    // at once.
    if (sock_ != kBadSock && localPort_ != 0) {
        sockaddr_in self{};
        self.sin_family = AF_INET;
        self.sin_port = ::htons(localPort_);
        ::inet_pton(AF_INET, "127.0.0.1", &self.sin_addr);
        const SockT s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (static_cast<std::intptr_t>(s) != kBadSock) {
            char c = 0;
            ::sendto(s, &c, 1, 0, reinterpret_cast<sockaddr*>(&self), sizeof(self));
            closeSock(static_cast<std::intptr_t>(s));
        }
    }
}

void UdpTransport::close() {
    shut_.store(true);
    if (sock_ != kBadSock) {
        closeSock(sock_);
        sock_ = kBadSock;
    }
}

// ---------------------------------------------------------------------------------------
// NetLink
// ---------------------------------------------------------------------------------------

void NetLink::pushSamples(const std::complex<float>* s, std::size_t n) {
    if (n == 0) { return; }
    received.fetch_add(n, std::memory_order_relaxed);
    bool kept = false;
    {
        std::lock_guard<std::mutex> gate(gateMutex);
        if (streaming.load(std::memory_order_relaxed)) {
            const std::size_t w = ring.write(s, n);
            if (w < n) { dropped.fetch_add(1, std::memory_order_relaxed); }
            kept = w > 0;
        }
    }
    if (kept) {
        std::lock_guard<std::mutex> lk(waitMutex);
        waitCv.notify_all();
    }
}

void NetLink::fault(const std::string& what, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(errorMutex);
        if (!faulted) {
            faulted = true;
            faultWhat = what;
        }
        lastError = what + ": " + detail;
    }
    run.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(waitMutex);
    waitCv.notify_all();
}

// ---------------------------------------------------------------------------------------
// NetSampleSource
// ---------------------------------------------------------------------------------------

NetSampleSource::~NetSampleSource() { endReader(); }

unsigned long long NetSampleSource::readersAbandoned() {
    return g_abandoned.load(std::memory_order_relaxed);
}

bool NetSampleSource::beginReader(Pump pump, std::function<void()> wake,
                                  std::shared_ptr<void> keepAlive) {
    endReader();
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        link_->faulted = false;
        link_->faultWhat.clear();
        link_->lastError.clear();
    }
    link_->exited = false;
    link_->run.store(true, std::memory_order_relaxed);
    wake_ = std::move(wake);
    std::shared_ptr<NetLink> link = link_;
    try {
        reader_ = std::thread([link, pump = std::move(pump), keepAlive = std::move(keepAlive)] {
            try {
                while (link->run.load(std::memory_order_relaxed)) {
                    if (!pump(*link)) { break; }
                }
            } catch (const std::exception& e) {
                link->fault("the sample reader failed", e.what());
            } catch (...) {
                link->fault("the sample reader failed", "an unexpected error");
            }
            {
                std::lock_guard<std::mutex> lk(link->waitMutex);
                link->exited = true;
            }
            link->waitCv.notify_all();
        });
    } catch (...) {
        link_->run.store(false);
        setError("could not start the sample reader thread");
        return false;
    }
    return true;
}

void NetSampleSource::endReader() {
    stopKeeping();
    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();
    if (reader_.joinable()) {
        if (wake_) { wake_(); }
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, std::chrono::milliseconds(kReaderJoinMs),
                                            [this] { return link_->exited; });
        }
        if (exited) {
            reader_.join();
        } else {
            g_abandoned.fetch_add(1, std::memory_order_relaxed);
            core::health::raiseRecovered(core::health::Recovered::ReaderLeak);
            link_->fault("waiting for the sample reader to stop",
                         "the reader did not return; FoxSDR must be restarted to use this "
                         "source again");
            reader_.detach();
        }
    }
    wake_ = nullptr;
    running_.store(false, std::memory_order_relaxed);
}

bool NetSampleSource::startKeeping() {
    if (faulted()) { return false; }
    {
        std::lock_guard<std::mutex> gate(link_->gateMutex);
        {
            std::lock_guard<std::mutex> c(link_->consumerMutex);
            std::vector<std::complex<float>> sink(4096);
            while (link_->ring.read(sink.data(), sink.size()) > 0) {}
        }
        link_->streaming.store(true, std::memory_order_relaxed);
    }
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void NetSampleSource::stopKeeping() {
    {
        std::lock_guard<std::mutex> gate(link_->gateMutex);
        link_->streaming.store(false, std::memory_order_relaxed);
    }
    running_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(link_->waitMutex);
    link_->waitCv.notify_all();
}

void NetSampleSource::stop() { stopKeeping(); }

std::size_t NetSampleSource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    const auto take = [this, dst, n] {
        std::lock_guard<std::mutex> lk(link_->consumerMutex);
        return link_->ring.read(dst, n);
    };
    std::size_t got = take();
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, std::chrono::milliseconds(kReadWaitMs), [this] {
            return link_->ring.size() > 0 || !link_->streaming.load(std::memory_order_relaxed) ||
                   !link_->run.load(std::memory_order_relaxed);
        });
    }
    return take();
}

bool NetSampleSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

std::string NetSampleSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faultWhat;
}

const char* NetSampleSource::lastError() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->lastError.c_str();
}

void NetSampleSource::setError(const std::string& msg) {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError = msg;
}

const char* NetSampleSource::name() const {
    std::lock_guard<std::mutex> lk(nameMutex_);
    return name_.c_str();
}

void NetSampleSource::setName(const std::string& n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = n;
}

}  // namespace cascade::source
