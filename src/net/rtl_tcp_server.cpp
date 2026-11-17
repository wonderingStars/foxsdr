// rtl_tcp_server.cpp - see rtl_tcp_server.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/rtl_tcp_server.hpp"

#include <algorithm>
#include <chrono>

#include "net/iq_out.hpp"

namespace cascade::net {
namespace {

constexpr std::size_t kChunkComplex = 4096;                 // complex samples per ring record (8192 bytes)
constexpr std::size_t kReadBudgetPerTurn = 64 * 1024;       // what the client may make the IO thread read in one turn

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

void RtlTcpServer::header(std::uint32_t tunerType, std::uint32_t gainCount, std::uint8_t out[kRtlTcpHeaderBytes]) noexcept {
    out[0] = 'R';
    out[1] = 'T';
    out[2] = 'L';
    out[3] = '0';
    put32(out + 4, tunerType);
    put32(out + 8, gainCount);
}

RtlTcpServer::~RtlTcpServer() { stop(); }

bool RtlTcpServer::start(CommandFn onCommand, std::string& error) {
    error.clear();
    if (running_.load()) {
        error = "already running";
        return false;
    }
    if (cfg_.clientQueueBytes < 64 * 1024 || cfg_.clientQueueBytes > (64u << 20)) {
        error = "the client queue must be between 64 KiB and 64 MiB";
        return false;
    }
    if (cfg_.ringBytes < 64 * 1024) { cfg_.ringBytes = 64 * 1024; }
    if (cfg_.pollMs < 1 || cfg_.pollMs > 1000) {
        error = "the poll interval must be between 1 and 1000 ms";
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
    if (!sock::ensureStarted()) {
        error = "could not initialise the network stack";
        return false;
    }
    std::vector<std::uint8_t> sink(ring_.maxMessage());
    while (ring_.pop(sink.data(), sink.size()) != 0) {}
    listener_ = sock::listenTcp(bindIp_, cfg_.port, 4, error);
    if (listener_ == sock::kInvalid) { return false; }
    onCommand_ = std::move(onCommand);
    port_.store(sock::boundPort(listener_), std::memory_order_release);
    stopFlag_.store(false);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void RtlTcpServer::stop() {
    if (!thread_.joinable()) { return; }
    stopFlag_.store(true);
    thread_.join();
    running_.store(false, std::memory_order_release);
    onCommand_ = nullptr;
}

std::size_t RtlTcpServer::submit(const float* iq, std::size_t nComplex) noexcept {
    if (!running_.load(std::memory_order_acquire) || clients_.load(std::memory_order_acquire) == 0 || iq == nullptr || nComplex == 0) { return 0; }
    samplesIn_.fetch_add(nComplex, std::memory_order_relaxed);
    std::uint8_t buf[kChunkComplex * 2];
    std::size_t queued = 0;
    while (queued < nComplex) {
        const std::size_t n = std::min(kChunkComplex, nComplex - queued);
        iqConvert(IqFormat::U8, iq + 2 * queued, n, buf);
        if (!ring_.push(buf, n * 2)) {
            samplesDropped_.fetch_add(nComplex - queued, std::memory_order_relaxed);
            return queued;
        }
        queued += n;
    }
    return queued;
}

RtlTcpStats RtlTcpServer::stats() const {
    RtlTcpStats s;
    s.accepted = accepted_.load();
    s.refusedBusy = refusedBusy_.load();
    s.refusedPeer = refusedPeer_.load();
    s.refusedProtocol = refusedProtocol_.load();
    s.closedByPeer = closedByPeer_.load();
    s.droppedStalled = droppedStalled_.load();
    s.bytesSent = bytesSent_.load();
    s.bytesRead = bytesRead_.load();
    s.samplesIn = samplesIn_.load();
    s.samplesDropped = samplesDropped_.load();
    s.commands = commands_.load();
    s.unknownCommands = unknownCommands_.load();
    s.clients = clients_.load();
    return s;
}

void RtlTcpServer::run() {
    std::vector<std::uint8_t> msg(ring_.maxMessage());
    std::vector<std::uint8_t> out;       // the one client's unsent bytes
    std::size_t off = 0;
    sock::Handle client = sock::kInvalid;
    std::int64_t lastProgress = 0;
    std::uint8_t cmd[kRtlTcpCommandBytes];
    std::size_t cmdFill = 0;
    bool anyCommand = false;   // a command has been read on this connection
    std::uint8_t trash[4096];

    const auto dropClient = [&](std::atomic<std::uint64_t>& why) {
        why.fetch_add(1, std::memory_order_relaxed);
        sock::closeHandle(client);
        client = sock::kInvalid;
        out.clear();
        off = 0;
        cmdFill = 0;
        anyCommand = false;
        clients_.store(0, std::memory_order_release);
    };
    const auto flush = [&](std::int64_t now) {   // false: the client is gone
        while (off < out.size()) {
            const long r = sock::sendSome(client, out.data() + off, out.size() - off);
            if (r > 0) {
                off += static_cast<std::size_t>(r);
                lastProgress = now;
                bytesSent_.fetch_add(static_cast<std::uint64_t>(r), std::memory_order_relaxed);
                continue;
            }
            if (r < 0 && sock::wouldBlock()) { break; }
            return false;
        }
        if (off == out.size()) { out.clear(); off = 0; }
        else if (off > 256 * 1024) { out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(off)); off = 0; }
        return true;
    };

    while (!stopFlag_.load(std::memory_order_acquire)) {
        std::int64_t now = nowMs();
        // 1. what the producer has handed over: to the client's queue, or thrown away when it is full or nobody is there
        for (;;) {
            const std::size_t n = ring_.pop(msg.data(), msg.size());
            if (n == 0) { break; }
            if (client == sock::kInvalid) { continue; }
            if (out.size() - off + n > cfg_.clientQueueBytes) {
                if (!flush(now)) { dropClient(closedByPeer_); continue; }
                if (out.size() - off + n > cfg_.clientQueueBytes) {
                    samplesDropped_.fetch_add(n / 2, std::memory_order_relaxed);   // the queue is full: the samples are dropped, the client stays
                    continue;
                }
            }
            out.insert(out.end(), msg.data(), msg.data() + n);
        }
        // 2. push what is queued, and judge a stall
        if (client != sock::kInvalid) {
            if (!flush(now)) { dropClient(closedByPeer_); }
            else if (cfg_.stallMs > 0 && off < out.size() && now - lastProgress >= cfg_.stallMs) { dropClient(droppedStalled_); }
        }
        // 3. wait for something to do
        sock::PollItem items[2];
        items[0].s = listener_;
        items[0].want = sock::kReadable;
        std::size_t count = 1;
        if (client != sock::kInvalid) {
            items[1].s = client;
            items[1].want = sock::kReadable | (off < out.size() ? sock::kWritable : 0u);
            count = 2;
        }
        if (sock::pollMany(items, count, cfg_.pollMs) < 0) { continue; }
        now = nowMs();
        if (items[0].got & sock::kReadable) {
            for (;;) {
                const sock::Handle h = sock::acceptOne(listener_);
                if (h == sock::kInvalid) { break; }
                if (!cfg_.allowedPeers.empty() && !cfg_.allowedPeers.allowsText(sock::peerOf(h))) {
                    refusedPeer_.fetch_add(1, std::memory_order_relaxed);   // not a computer the person listed: it never gets the place, nor a byte
                    sock::closeHandle(h);
                    continue;
                }
                if (client != sock::kInvalid) {
                    refusedBusy_.fetch_add(1, std::memory_order_relaxed);
                    sock::closeHandle(h);
                    continue;
                }
                sock::setNoDelay(h);
                client = h;
                out.assign(kRtlTcpHeaderBytes, 0);
                header(tunerType_.load(std::memory_order_relaxed), gainCount_.load(std::memory_order_relaxed), out.data());
                off = 0;
                cmdFill = 0;
                anyCommand = false;
                lastProgress = now;
                accepted_.fetch_add(1, std::memory_order_relaxed);
                clients_.store(1, std::memory_order_release);
            }
        }
        // 4. the client: read what it sends (only whole commands are looked at), write on readiness
        if (client != sock::kInvalid && count == 2 && items[1].got != 0) {
            bool gone = false;
            if (items[1].got & sock::kReadable) {
                std::size_t budget = kReadBudgetPerTurn;
                while (budget > 0) {
                    std::uint8_t b[512];
                    const long r = sock::recvSome(client, b, std::min<std::size_t>(sizeof(b), budget));
                    if (r > 0) {
                        bytesRead_.fetch_add(static_cast<std::uint64_t>(r), std::memory_order_relaxed);
                        budget -= static_cast<std::size_t>(r);
                        for (long i = 0; i < r; ++i) {
                            cmd[cmdFill++] = b[i];
                            if (cmdFill == kRtlTcpCommandBytes) {
                                cmdFill = 0;
                                commands_.fetch_add(1, std::memory_order_relaxed);
                                if (!anyCommand && !(cmd[0] >= 0x01 && cmd[0] <= kRtlCmdLast)) {
                                    // A program that speaks rtl_tcp starts with an rtl_tcp command. What starts with anything else is another protocol aimed at this port (a web page's request to
                                    // http://127.0.0.1:1234, which the browser lets through, begins with a letter): it is dropped before it can line up a later 5-byte block as a retune.
                                    refusedProtocol_.fetch_add(1, std::memory_order_relaxed);
                                    gone = true;
                                    break;
                                }
                                anyCommand = true;
                                if (cmd[0] >= 0x01 && cmd[0] <= kRtlCmdLast) {
                                    if (onCommand_) { onCommand_(cmd[0], get32(cmd + 1)); }
                                } else {
                                    unknownCommands_.fetch_add(1, std::memory_order_relaxed);
                                }
                            }
                        }
                        if (gone) { break; }
                        continue;
                    }
                    if (r < 0 && sock::wouldBlock()) { break; }
                    gone = true;
                    break;
                }
            }
            if (!gone && (items[1].got & sock::kError)) { gone = true; }
            if (!gone && (items[1].got & sock::kWritable) && !flush(now)) { gone = true; }
            if (gone) { dropClient(closedByPeer_); }
        }
        (void)trash;
    }
    if (client != sock::kInvalid) { sock::closeHandle(client); }
    clients_.store(0, std::memory_order_release);
    sock::closeHandle(listener_);
    listener_ = sock::kInvalid;
}

}  // namespace cascade::net
