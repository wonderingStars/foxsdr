// tcp_fanout.cpp - see tcp_fanout.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/tcp_fanout.hpp"

#include <algorithm>
#include <chrono>

namespace cascade::net {
namespace {

// What one client may make the IO thread read and discard in one turn (a flood cannot starve the other clients).
constexpr std::size_t kIgnoreBudgetPerTurn = 64 * 1024;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

TcpFanout::TcpFanout(const FanoutConfig& cfg) : cfg_(cfg), ring_(std::max<std::size_t>(cfg.ringBytes, 4096)) {}

TcpFanout::~TcpFanout() { stop(); }

std::size_t TcpFanout::maxMessage() const { return std::min(ring_.maxMessage(), cfg_.clientQueueBytes); }

bool TcpFanout::start(std::string& error) {
    if (running_.load()) {
        error = "already running";
        return false;
    }
    if (cfg_.maxClients < 1 || cfg_.maxClients > kFanoutMaxClientsCap) {
        error = "client limit must be between 1 and " + std::to_string(kFanoutMaxClientsCap);
        return false;
    }
    if (cfg_.clientQueueBytes < 1024) {
        error = "per-client queue must be at least 1024 bytes";
        return false;
    }
    if (cfg_.greeting.size() >= cfg_.clientQueueBytes) {
        error = "greeting does not fit the per-client queue";
        return false;
    }
    if (cfg_.pollMs < 1 || cfg_.pollMs > 1000) {
        error = "poll interval must be between 1 and 1000 ms";
        return false;
    }
    if (!sock::parseIpv4(cfg_.bindAddress, bindIp_)) {
        error = "'" + cfg_.bindAddress + "' is not an IPv4 address (dotted quad, for example 127.0.0.1)";
        return false;
    }
    if (bindIp_ == 0 && !cfg_.allowAllInterfaces) {
        error = "0.0.0.0 would expose this stream on every network interface; it is refused unless that is asked for explicitly";
        return false;
    }
    // A previous run may have left records behind; this thread is the only consumer now.
    std::vector<std::uint8_t> sink(ring_.maxMessage());
    while (ring_.pop(sink.data(), sink.size()) != 0) {}

    listener_ = sock::listenTcp(bindIp_, cfg_.port, 16, error);
    if (listener_ == sock::kInvalid) { return false; }
    port_.store(sock::boundPort(listener_), std::memory_order_release);
    stopFlag_.store(false);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void TcpFanout::stop() {
    if (!thread_.joinable()) { return; }
    stopFlag_.store(true);
    thread_.join();
    running_.store(false, std::memory_order_release);
}

bool TcpFanout::publish(const void* data, std::size_t n) noexcept {
    if (!running_.load(std::memory_order_acquire)) { return false; }
    if (n > cfg_.clientQueueBytes) {
        // Could not be queued for any client, whatever the ring would say.
        oversize_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!ring_.push(data, n)) { return false; }
    published_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

FanoutStats TcpFanout::stats() const {
    FanoutStats s;
    s.accepted = accepted_.load();
    s.refusedFull = refusedFull_.load();
    s.droppedSlow = droppedSlow_.load();
    s.droppedStalled = droppedStalled_.load();
    s.closedByPeer = closedByPeer_.load();
    s.published = published_.load();
    s.ringDropped = ring_.dropped();
    s.oversize = ring_.oversize() + oversize_.load();
    s.bytesSent = bytesSent_.load();
    s.bytesIgnored = bytesIgnored_.load();
    s.clients = clientCount_.load();
    return s;
}

void TcpFanout::drop(std::size_t index, std::atomic<std::uint64_t>& why) {
    // Counters first, then the close: whoever sees the connection end can then also see why it ended.
    const sock::Handle h = clients_[index].s;
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
    clientCount_.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    why.fetch_add(1, std::memory_order_relaxed);
    sock::closeHandle(h);
}

bool TcpFanout::flush(Client& c, std::int64_t now) {
    while (c.pending() > 0) {
        const long r = sock::sendSome(c.s, c.out.data() + c.off, c.pending());
        if (r > 0) {
            c.off += static_cast<std::size_t>(r);
            c.lastProgressMs = now;
            bytesSent_.fetch_add(static_cast<std::uint64_t>(r), std::memory_order_relaxed);
            continue;
        }
        if (r < 0 && sock::wouldBlock()) { break; }
        return false;  // 0 or a hard error
    }
    if (c.off == c.out.size()) {
        c.out.clear();
        c.off = 0;
    } else if (c.off > 64 * 1024) {
        c.out.erase(c.out.begin(), c.out.begin() + static_cast<std::ptrdiff_t>(c.off));
        c.off = 0;
    }
    return true;
}

void TcpFanout::dispatch(const std::uint8_t* data, std::size_t n) {
    const std::int64_t now = nowMs();
    for (std::size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        if (c.pending() + n > cfg_.clientQueueBytes) {
            // Try to make room first: the socket may simply not have been polled yet.
            if (!flush(c, now)) {
                drop(i, closedByPeer_);
                continue;
            }
            if (c.pending() + n > cfg_.clientQueueBytes) {
                drop(i, droppedSlow_);
                continue;
            }
        }
        if (c.pending() == 0) { c.lastProgressMs = now; }
        c.out.insert(c.out.end(), data, data + n);
        ++i;
    }
}

void TcpFanout::acceptAll(std::int64_t now) {
    for (;;) {
        const sock::Handle h = sock::acceptOne(listener_);
        if (h == sock::kInvalid) { return; }
        if (static_cast<int>(clients_.size()) >= cfg_.maxClients) {
            refusedFull_.fetch_add(1, std::memory_order_relaxed);
            sock::closeHandle(h);
            continue;
        }
        sock::setNoDelay(h);
        if (cfg_.socketSendBuffer > 0) { sock::setBuffers(h, 0, cfg_.socketSendBuffer); }
        Client c;
        c.s = h;
        c.lastProgressMs = now;
        c.out = cfg_.greeting;
        clients_.push_back(std::move(c));
        accepted_.fetch_add(1, std::memory_order_relaxed);
        clientCount_.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    }
}

void TcpFanout::run() {
    std::vector<std::uint8_t> msg(ring_.maxMessage());
    std::vector<sock::PollItem> items;
    std::uint8_t trash[4096];
    while (!stopFlag_.load(std::memory_order_acquire)) {
        std::int64_t now = nowMs();
        // 1. everything the producer has handed over
        for (;;) {
            const std::size_t n = ring_.pop(msg.data(), msg.size());
            if (n == 0) { break; }
            if (n <= cfg_.clientQueueBytes) { dispatch(msg.data(), n); }
        }
        // 2. push what is queued, and judge stalls
        for (std::size_t i = 0; i < clients_.size();) {
            if (!flush(clients_[i], now)) {
                drop(i, closedByPeer_);
                continue;
            }
            if (cfg_.stallMs > 0 && clients_[i].pending() > 0 && now - clients_[i].lastProgressMs >= cfg_.stallMs) {
                drop(i, droppedStalled_);
                continue;
            }
            ++i;
        }
        // 3. wait for something to do
        items.clear();
        sock::PollItem l;
        l.s = listener_;
        l.want = sock::kReadable;
        items.push_back(l);
        for (const Client& c : clients_) {
            sock::PollItem it;
            it.s = c.s;
            it.want = sock::kReadable | (c.pending() > 0 ? sock::kWritable : 0);
            items.push_back(it);
        }
        if (sock::pollMany(items.data(), items.size(), cfg_.pollMs) < 0) { continue; }
        now = nowMs();
        if (items[0].got & sock::kReadable) { acceptAll(now); }
        // 4. clients: read-and-discard, detect close, write on readiness. Walk backwards so a drop keeps the indices ahead valid.
        const std::size_t polled = items.size() - 1;
        for (std::size_t k = polled; k-- > 0;) {
            if (k >= clients_.size()) { continue; }
            const unsigned got = items[k + 1].got;
            if (got == 0) { continue; }
            Client& c = clients_[k];
            bool gone = false;
            if (got & sock::kReadable) {
                std::size_t budget = kIgnoreBudgetPerTurn;
                while (budget > 0) {
                    const long r = sock::recvSome(c.s, trash, sizeof(trash));
                    if (r > 0) {
                        bytesIgnored_.fetch_add(static_cast<std::uint64_t>(r), std::memory_order_relaxed);
                        budget -= std::min<std::size_t>(budget, static_cast<std::size_t>(r));
                        continue;
                    }
                    if (r < 0 && sock::wouldBlock()) { break; }
                    gone = true;
                    break;
                }
            }
            if (!gone && (got & sock::kError)) { gone = true; }
            if (!gone && (got & sock::kWritable) && !flush(c, now)) { gone = true; }
            if (gone) { drop(k, closedByPeer_); }
        }
    }
    for (Client& c : clients_) { sock::closeHandle(c.s); }
    clients_.clear();
    clientCount_.store(0, std::memory_order_relaxed);
    sock::closeHandle(listener_);
    listener_ = sock::kInvalid;
}

}  // namespace cascade::net
