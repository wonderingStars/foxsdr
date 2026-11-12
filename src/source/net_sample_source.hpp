// net_sample_source.hpp - the shared body of the network radio sources of this lane
// (SoapyRemote, SDRconnect, HPSDR): a DeviceSource whose samples arrive on a reader thread
// from a network transport and are handed to the pipeline through a ring.
//
// WHAT IS SHARED, AND WHAT IS NOT. Shared: the ring, the "keep or discard" gate that start()
// and stop() flip, the fault latch, the stream-health tally, and a reader thread that is
// joined with a bound and ABANDONED (not killed, not closed under) when it does not return.
// Not shared: every byte of every protocol. A derived class owns its control connection and
// hands this class a PUMP, a function called again and again on the reader thread that
// receives one unit of the protocol and pushes the samples it carries with pushSamples().
//
// THE READER OUTLIVES THIS OBJECT WHEN IT HAS TO, the argument RtlTcpSource and PlutoSource
// make: the thread captures a shared_ptr to its link and to the transport and nothing else,
// so a reader that does not return in time is detached and keeps everything it touches alive
// until it does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "source/device_source.hpp"
#include "source/iiod_client.hpp"

namespace cascade::source {

// "host", "host:port", "[v6]:port" or a bare IPv6 literal; `defaultPort` when none is given. False
// with a sentence in `error` for an empty host, an unclosed bracket or a port outside 1..65535.
bool parseHostPort(const std::string& text, std::uint16_t defaultPort, std::string& host,
                   std::uint16_t& port, std::string& error);

// A datagram socket as an iiod::Transport: recvSome() delivers ONE datagram (up to `max`
// bytes), sendAll() sends one datagram. recvAll() is not meaningful on a datagram socket and
// is implemented as "exactly one datagram of exactly n bytes". Bounded by ioWait on every
// receive. bind() is to 127.0.0.1-or-any local address on an ephemeral port when port is 0.
class UdpTransport : public iiod::Transport {
public:
    ~UdpTransport() override;
    // Opens an IPv4 UDP socket bound to `localHost`:`localPort` (port 0: ephemeral). Null with
    // `error` set on failure.
    static std::unique_ptr<UdpTransport> bindLocal(const std::string& localHost,
                                                   std::uint16_t localPort,
                                                   std::chrono::milliseconds ioWait,
                                                   std::string& error);
    // Sets the default peer (so send() needs no address and only that peer's datagrams are
    // received). False with lastError() set.
    bool connectTo(const std::string& host, std::uint16_t port);
    std::uint16_t localPort() const { return localPort_; }
    // Asks the OS for a larger receive queue (a reader that is descheduled for a few tens of ms on a busy
    // machine otherwise loses packets of a fast stream). Best effort; false if the OS refused.
    bool setReceiveBuffer(int bytes);

    bool sendAll(const void* data, std::size_t n) override;
    bool recvAll(void* data, std::size_t n) override;
    bool recvSome(void* data, std::size_t max, std::size_t& got) override;
    void shutdown() override;
    void close() override;
    const char* lastError() const override { return error_.c_str(); }

private:
    UdpTransport() = default;
    std::intptr_t sock_ = -1;
    std::uint16_t localPort_ = 0;
    std::chrono::milliseconds ioWait_{2000};
    std::atomic<bool> shut_{false};
    std::string error_;
};

// The state a reader thread shares with its source.
struct NetLink {
    explicit NetLink(std::size_t ringCapacity) : ring(ringCapacity) {}

    std::atomic<bool> run{false};
    std::atomic<bool> streaming{false};
    std::mutex gateMutex;      // held by pushSamples() while it decides and writes
    std::mutex consumerMutex;  // the consumer side of the ring: read() and the clear in start()
    cascade::dsp::SpscRing<std::complex<float>> ring;

    std::mutex waitMutex;
    std::condition_variable waitCv;
    bool exited = false;

    mutable std::mutex errorMutex;
    std::string lastError;
    bool faulted = false;
    std::string faultWhat;

    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> received{0};

    // Called by the pump. Keeps the samples when streaming, discards them otherwise; counts
    // what the ring could not take. Wakes read().
    void pushSamples(const std::complex<float>* s, std::size_t n);
    // Latches a fault with its sentence; the reader then ends.
    void fault(const std::string& what, const std::string& detail);
};

class NetSampleSource : public DeviceSource {
public:
    // Milliseconds, not std::chrono constants: the shutdown-budget scan classifies named
    // chrono waits per file; these are bounds spent on the teardown thread (join) and on the
    // pipeline's source thread (read), as RtlTcpSource's are.
    static constexpr int kReaderJoinMs = 2500;
    static constexpr int kReadWaitMs = 20;

    ~NetSampleSource() override;

    // --- the parts of the IqSource / DeviceSource contract that are the same everywhere -----
    bool running() const override { return running_.load(std::memory_order_relaxed); }
    bool selfPaced() const override { return true; }
    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    bool deviceDead() const override { return faulted(); }
    std::string faultedWhile() const override;
    const char* name() const override;
    const char* lastError() const override;
    void stop() override;

    std::uint64_t droppedBuffers() const { return link_->dropped.load(); }
    std::uint64_t samplesReceived() const { return link_->received.load(); }

    // Reader threads this PROCESS has abandoned. 0 on every healthy path.
    static unsigned long long readersAbandoned();

protected:
    NetSampleSource() = default;

    // One unit of the protocol, on the reader thread. Receive (bounded by the transport's own
    // wait), push samples, return true to be called again; false ends the reader (call
    // link.fault() first if it is a failure). Must touch nothing but its arguments.
    using Pump = std::function<bool(NetLink&)>;

    // Starts the reader. `wake` is called (from the closing thread) to end a receive in
    // progress: normally transport->shutdown(). `keepAlive` is held by the reader thread for
    // as long as it runs (the transports it uses).
    bool beginReader(Pump pump, std::function<void()> wake, std::shared_ptr<void> keepAlive);
    // Wakes, joins with the bound, or abandons. Idempotent.
    void endReader();

    // Begin / end KEEPING samples. startKeeping() clears the ring first.
    bool startKeeping();
    void stopKeeping();

    void setError(const std::string& msg);
    void setName(const std::string& n);
    void markOpen(bool open) { openMirror_.store(open, std::memory_order_relaxed); }
    std::shared_ptr<NetLink> link() const { return link_; }

    std::atomic<bool> running_{false};
    std::atomic<bool> openMirror_{false};

private:
    // NEVER REASSIGNED: an abandoned reader holds its own copy.
    const std::shared_ptr<NetLink> link_ = std::make_shared<NetLink>(1u << 19);
    std::thread reader_;
    std::function<void()> wake_;
    mutable std::mutex nameMutex_;
    std::string name_ = "network radio: (not connected)";
};

}  // namespace cascade::source
