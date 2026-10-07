// rtl_tcp_source.hpp - FoxSDR's client for the rtl_tcp server: an RTL-SDR (or a
// program that imitates one) somewhere on a network, streamed to this machine
// as 8-bit I/Q over TCP. A DeviceSource with no librtlsdr, no libusb and no
// SoapySDR module anywhere in the path: a socket, a twelve-byte header, and
// five-byte commands.
//
// WHY IT EXISTS. Asked for through the feature-request box on 2026-10-03
// ("can rtl_tcp be added as a source"). An RTL-SDR on a Raspberry Pi in the
// loft, a dongle on another PC, SDR++ Server's rtl_tcp-compatible mode and a
// cloud receiver are all reached this way, and none of them needed anything
// installed on this machine once this driver existed.
//
// PROVENANCE (CONTRIBUTING.md, clean room). The protocol was taken from its
// public description - the rtl_tcp server's documented behaviour: it accepts
// one TCP client (default port 1234), sends a twelve-byte header, streams
// unsigned 8-bit I/Q for ever, and reads five-byte commands each with a
// numbered id. librtlsdr's rtl_tcp.c is GPL and was NOT consulted: nothing
// here is copied or paraphrased from it, no librtlsdr header is included, and
// nothing links against it. The numbers below are facts about a wire format;
// the words around them and every line of code are this project's own. The
// wire format was VERIFIED against a real server on 2026-10-06: radioconda's
// rtl_tcp.exe serving an RTL2838 (R820T) on 127.0.0.1 - the header, the
// stream at 2.4 MS/s, and the fault when the server exited. The test suite
// itself uses only a fake written from this same description
// (tests/test_rtl_tcp_source.cpp says so too), so nothing in the suite proves
// what a real server sends.
//
// THE WIRE FORMAT, all integers big-endian:
//
//   server -> client, once, on connect (12 bytes):
//       "RTL0"                the magic
//       u32 tunerType         0 unknown, 1 E4000, 2 FC0012, 3 FC0013,
//                             4 FC2580, 5 R820T, 6 R828D
//       u32 gainCount         how many tuner gain steps the server's own
//                             tuner has (what 0x0d's index runs over)
//   server -> client, then for ever: unsigned 8-bit I, Q, I, Q ... at the
//                             rate last set (the server's default until the
//                             first 0x02)
//   client -> server, any time, 5 bytes each: u8 id, u32 value
//       0x01  centre frequency, Hz
//       0x02  sample rate, Hz
//       0x03  gain mode: 0 automatic, 1 manual
//       0x04  tuner gain in tenths of a dB (the server picks the nearest step)
//       0x05  frequency correction in ppm, a signed number sent as the u32
//             two's complement
//       0x08  the RTL2832's own digital AGC: 0 off, 1 on
//       0x0d  tuner gain by INDEX into the tuner's own table
//       0x0e  bias tee: 0 off, 1 on
//   The server NEVER ACKNOWLEDGES a command. A client cannot learn whether one
//   was understood, or what the server rounded it to; what this driver reports
//   back from gainDb(), sampleRateHz() and the rest is what it ASKED FOR,
//   which is the best a one-way protocol allows and is said so in the docs.
//
// ONE SOCKET CARRIES BOTH DIRECTIONS, which is the difference from the other
// network driver (pluto_source.hpp, two connections): a command is a send on
// the same socket the reader thread is parked receiving from. TCP is full
// duplex, so that is safe; what had to be made safe is the bookkeeping, and
// the transport keeps a separate error string for each direction
// (iiod_client.cpp, TcpTransport).
//
// THE SERVER KEEPS ITS OWN STATE BETWEEN CLIENTS. The server process owns the
// dongle, and a dongle does not forget: the frequency correction, the gain
// mode and the bias tee a PREVIOUS client left are still set. So the
// connection sends the complete state this driver means to have - rate,
// frequency, correction, digital AGC off, gain mode, gain, bias tee - rather
// than trusting the server's. Rate and frequency go first because some
// servers do not begin streaming until they have seen them.
//
// ONE CONNECTION, FROM open() TO closeDevice(). The socket and the reader
// thread live for the whole of an open; stop() and start() only pause and
// resume the KEEPING of samples. While stopped the reader goes on draining the
// socket and throws what it receives away, so the server is never
// back-pressured, stop() returns at once whatever the network is doing, and
// start() has no connection to make: nothing is reconnected or replayed (the
// server's state never changed - every setter sends at once, stopped or not).
// Two consequences, both said in the README: rtl_tcp serves ONE client at a
// time, so the server stays busy until the source is CLOSED (not merely
// stopped); and a stopped source still receives the full sample stream from the
// server and discards it.
//
// NO AUTOMATIC RECONNECT AFTER A FAULT, deliberately and for the reason the
// other drivers give: a connection that died mid-stream is reported (the
// pipeline polls faulted() and says "Device stopped") and the user opens it
// again, rather than the receiver quietly retrying a server that has gone.
//
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT: as PlutoSource (read its header
// for the argument). One reader thread receives into a ring; its state lives
// in a ReaderLink behind a shared_ptr the thread captures by value; the join is
// bounded and a reader that does not return is abandoned, its socket leaked
// deliberately rather than closed under it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "source/device_source.hpp"
#include "source/iiod_client.hpp"

namespace cascade::source {

// The port rtl_tcp listens on unless told otherwise.
constexpr std::uint16_t kRtlTcpDefaultPort = 1234;

// The host and port an args string names: "rtltcp=192.168.1.20:1234",
// "rtltcp=sdr.local" (default port), "rtltcp=[fe80::1]:1234" (an IPv6 literal
// in brackets, as in a URL) or a bare IPv6 literal with no port. An empty
// value, or no key at all, is the server's own default address, 127.0.0.1:1234
// - the same "nothing typed means the usual place" rule the Pluto's address
// follows. False with a sentence in `error` for a port that is not a number in
// 1..65535 or an unclosed bracket.
bool parseRtlTcpAddress(const std::string& args, std::string& host, std::uint16_t& port,
                        std::string& error);

// What a tuner type number from the header names. "unknown tuner" for 0 and
// for a number this table does not know - a newer server may report one.
const char* rtlTcpTunerName(std::uint32_t tunerType);

class RtlTcpSource : public DeviceSource {
public:
    // --- the bounded waits ------------------------------------------------
    //
    // CLASS MEMBERS, for the reason PlutoSource's header gives (hackrf_source
    // already owns these names at namespace scope). The connect bound and the
    // socket's send/receive bound are iiod::kConnectWait and iiod::kReplyWait:
    // the same socket code, so the same two constants.

    // The bound on joining the reader thread. Longer than iiod::kReplyWait on
    // purpose - the reader's longest legitimate stall is one socket receive,
    // and a shorter join would abandon a thread that was coming back.
    static constexpr std::chrono::milliseconds kReaderJoinWait{2500};

    // How long read() waits for the reader to put something in the ring
    // before returning the IqSource contract's "nothing yet, retry" zero.
    static constexpr std::chrono::milliseconds kReadWait{20};

    // The stream-health window, matching the other drivers'. Not a wait:
    // nothing sleeps on it. It is how much streaming the reader tallies before
    // it writes one "source: stream health ..." line.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    RtlTcpSource() = default;
    ~RtlTcpSource() override;

    RtlTcpSource(const RtlTcpSource&) = delete;
    RtlTcpSource& operator=(const RtlTcpSource&) = delete;

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "rtltcp"; }

    // Connects (bounded by iiod::kConnectWait), reads and checks the header,
    // sends the initial state (see the file header), leaves the tuner in
    // MANUAL gain at a middling setting - the launchDeviceOpen order
    // (open(), a default sample rate, then adoptDeviceMirrors, which switches
    // automatic gain off and reads the gain back) needs exactly that - and
    // starts the reader, which keeps no sample until start(). A source that
    // has lost its connection refuses to open again: the receiver makes a
    // fresh one for every open.
    bool open(const std::string& args) override;

    // TEST SEAM: open() with the connected transport supplied instead of
    // dialled, so a test can script exactly what arrives (chunks of chosen
    // sizes, a receive that never returns). Everything after the dial -
    // header, state, reader - is the same code. The args still name the
    // address (it appears only in open errors).
    bool openWithTransportForTest(const std::string& args,
                                  std::unique_ptr<iiod::Transport> transport);

    // Wakes the reader (shutdown() on the socket), joins it (bounded by
    // kReaderJoinWait, abandoning it as the Pluto's does) and closes the
    // socket, which is what frees the server for other clients. Idempotent,
    // safe on a never-opened instance and from the destructor. lastError()
    // survives it.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // ONE GAIN, "TUNER".
    //  * An R820T / R828D (types 5 and 6): decibels, on the same ladder the
    //    native RTL-SDR driver uses (TunerR82xx::aggregateLadderTenthDb); a
    //    request is snapped to the nearest rung and sent with 0x04.
    //  * Any other tuner: STEPS, 0 to gainCount-1, sent with 0x0d. The server
    //    does not publish decibel values for those and none is invented here
    //    (the Airspy R2's rule, device_source.hpp GainUnit). A server that
    //    reports zero steps gets a 0..0 range and nothing is sent.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // Supported: 0x03 with 0 or 1. Turning it off re-sends the manual gain so
    // the manual level is applied rather than whatever the server kept.
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return autoGain_.load(std::memory_order_relaxed); }

    // None: there is one input and the protocol has no word for choosing.
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return std::string(); }

    // The RTL-SDR menu (Rtl2832u::supportedRatesHz), reused rather than
    // retyped: the server's dongle is the same chip.
    std::vector<double> supportedSampleRatesHz() const override;

    // 24 MHz to 1766 MHz for an R820T/R828D and for a tuner the header does
    // not name; 52 MHz to 2200 MHz for an E4000. INFORMATION, not a limit:
    // setCenterFrequencyHz sends whatever fits a 32-bit Hz value, because the
    // server's dongle may have direct sampling or an up-converter (an RTL-SDR
    // Blog V4 reaches the HF bands) that this client cannot know about.
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // Begins KEEPING samples: clears whatever the ring still holds (so nothing
    // received before a stop is ever delivered after the start) and opens the
    // gate. Returns at once - no connection is made, nothing is replayed. False
    // when nothing is open or the connection is dead.
    bool start() override;

    // Stops keeping samples and returns at once, with no join and no network
    // call. The connection stays open and the reader keeps draining it (see
    // the file header). Idempotent.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Snapped to the nearest rate of supportedSampleRatesHz() and sent with
    // 0x02, stopped or running. Works straight after open() and on a running
    // stream; the few milliseconds of samples already in flight at the old rate are delivered
    // before the new rate takes hold - the server cannot be asked to mark the
    // change.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }
    bool setCenterFrequencyHz(double hz) override;

    // The remote dongle's own crystal trim: whole ppm (0x05, the u32 two's
    // complement), followed by a re-send of the centre frequency so the dongle
    // is left ON the frequency this driver reports whatever the server does
    // with the correction.
    bool hasFrequencyCorrection() const override { return true; }
    bool setFrequencyCorrectionPpm(double ppm) override;

    // Bias tee (0x0e), spelled like every native driver's pair so
    // gui/bias_tee.hpp's dynamic_cast chain reaches it. The readback is the
    // REQUEST: the server never says whether its dongle has one.
    bool setBiasT(bool on);
    bool biasT() const { return biasTee_.load(std::memory_order_relaxed); }

    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    bool faulted() const override;
    const char* name() const override;
    const char* lastError() const override;

    // --- what the header said ---------------------------------------------
    std::uint32_t tunerType() const { return tunerType_.load(std::memory_order_relaxed); }
    std::uint32_t tunerGainCount() const { return gainCount_.load(std::memory_order_relaxed); }
    std::string tunerName() const { return rtlTcpTunerName(tunerType()); }

    // --- stream health ---------------------------------------------------
    //
    // The same tally and the same one-line format the other drivers write.
    struct StreamHealth {
        std::uint64_t reads = 0;
        std::uint64_t withSamples = 0;
        std::uint64_t samples = 0;
        std::uint64_t timeouts = 0;
        std::uint64_t overflows = 0;
        std::uint64_t errors = 0;
        std::int64_t longestGapMs = 0;
        bool windowOpen = false;
        std::chrono::steady_clock::time_point windowStart{};
        std::chrono::steady_clock::time_point lastSamples{};
    };
    std::string streamHealthLine();
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // Receives the host could not take because the ring was full.
    std::uint64_t droppedBuffers() const;

    // Reader threads this PROCESS has abandoned: 0 on every healthy path, and
    // the delta is what a test asserts.
    static unsigned long long readersAbandoned();

private:
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        // The reader's own loop flag: true from open() to closeDevice().
        std::atomic<bool> run{false};

        // Whether samples are KEPT (start() to stop()); otherwise the reader
        // receives and discards. Written under gateMutex, which the reader
        // holds while it decides and writes, so a flip is atomic with it.
        std::atomic<bool> streaming{false};
        std::mutex gateMutex;
        // The consumer side of the ring: read() and start()'s clear.
        std::mutex consumerMutex;

        // The socket. NOT owned here: the source owns it, except on the
        // abandoning path, which leaks it so a stranded reader still has a
        // live object to be inside.
        iiod::Transport* transport = nullptr;

        cascade::dsp::SpscRing<std::complex<float>> ring;

        std::mutex waitMutex;
        std::condition_variable waitCv;
        bool exited = false;

        mutable std::mutex errorMutex;
        std::string lastError;
        bool faulted = false;
        bool deviceDead = false;
        std::string deadWhat;

        mutable std::mutex healthMutex;
        StreamHealth health;
        bool healthEverWritten = false;
        std::chrono::milliseconds healthWindow = kStreamHealthWindow;
        std::atomic<std::uint64_t> dropped{0};
    };

    // NEVER REASSIGNED: an abandoned reader holds its own copy.
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    // 2^19 samples: 164 ms at the top rate (3.2 MS/s), 4 MiB - generous
    // because a network hands samples over in bursts a USB bulk pipe does not.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 19; }

    bool openImpl(const std::string& args, std::unique_ptr<iiod::Transport> injected);

    // The *Locked helpers assume devMutex_ is held.
    std::unique_ptr<iiod::Transport> dialLocked(std::string& error);
    // Read and check the header, choose the gain, send the whole state.
    bool handshakeLocked(std::unique_ptr<iiod::Transport> t, std::string& error);
    bool sendCommandLocked(std::uint8_t id, std::uint32_t value);
    bool sendStateLocked(std::string& error);
    bool sendGainLocked();
    void startReaderLocked();
    void stopReaderLocked();
    bool isR82xx() const;
    double snapGainDb(double db) const;

    static void readerThreadBody(std::shared_ptr<ReaderLink> link);
    static void noteRead(ReaderLink& link, bool ok, std::size_t samples, bool dropped);
    static std::string healthLineLocked(ReaderLink& link);

    static void setErrorOn(ReaderLink& link, std::string msg);
    static void noteFaultOn(ReaderLink& link, const char* what, const std::string& detail);
    void setError(std::string msg);
    void clearError();
    void noteFault(const char* what, const std::string& detail);
    void setName(std::string n);

    mutable std::mutex devMutex_;

    std::unique_ptr<iiod::Transport> transport_;
    std::thread reader_;

    std::string host_;
    std::uint16_t port_ = kRtlTcpDefaultPort;

    // WHAT THIS DRIVER MEANS THE SERVER TO BE DOING - sent whole when the
    // connection is made (see the file header). Under devMutex_.
    double rateHz_ = 2048000.0;
    double centerHz_ = 100000000.0;
    int ppm_ = 0;
    bool biasWanted_ = false;
    bool autoWanted_ = false;
    std::vector<int> ladderTenths_;  // the R82xx rungs, empty for other tuners
    int gainTenths_ = 0;             // the chosen rung, R82xx only
    int gainIndex_ = 0;              // the chosen index, other tuners

    std::atomic<std::uint32_t> tunerType_{0};
    std::atomic<std::uint32_t> gainCount_{0};

    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<double> gainValue_{0.0};
    std::atomic<bool> autoGain_{false};
    std::atomic<bool> biasTee_{false};

    mutable std::mutex nameMutex_;
    std::string name_ = "rtl_tcp: (not connected)";
};

}  // namespace cascade::source
