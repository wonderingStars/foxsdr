// pluto_tx.hpp - the ADALM-Pluto's transmit half, over the same IIOD network
// protocol its receive half uses. No libiio, no libad9361, no SoapySDR: the
// client in source/iiod_client.hpp, a second connection to the same daemon,
// and the one command the receive driver never sends - WRITEBUF.
//
// WHAT pluto_source.hpp WROTE DOWN, AND WHAT IT TURNED OUT TO BE. That header
// has carried a "WHAT IS NOT HERE: TRANSMIT" note since 0.93.0 listing four
// facts, and all four held: the DDS/DAC device is `cf-ad9361-dds-core-lpc`
// and its scan elements are on OUTPUT channels; the TX LO is the phy's
// out_altvoltage1 (named TX_LO against altvoltage0's RX_LO); the TX gain is
// out_voltage0 hardwaregain and is an ATTENUATION in negative dB; the TX port
// is out_voltage0 rf_port_select. This file is that note built, plus the two
// things it did not say: the DDS tone generators have to be turned OFF or the
// DAC plays them instead of the buffer, and WRITEBUF is a three-part
// conversation rather than a command (iiod_client.hpp's writeBuf says which
// three).
//
// ============================================================================
// THE SAFETY PROPERTY, and everything below is arranged around it
// ============================================================================
//
// A RECEIVER THAT MISBEHAVES SHOWS THE WRONG PICTURE. A TRANSMITTER THAT
// MISBEHAVES PUTS RF ON SOMEBODY ELSE'S BAND. So the whole of this class is
// built so that "quiet" is what happens by default and "transmitting" is a
// state something had to deliberately enter and keep entering:
//
//   - open() LEAVES THE BOARD QUIET, and it does not merely decline to make
//     it loud. A Pluto keeps whatever the last program left in it, so a board
//     handed over with its attenuation at 0 dB and its TX LO up is a board
//     that would start radiating the instant anything fed the DAC. open()
//     therefore writes the attenuation to the board's own MAXIMUM and powers
//     the TX LO down before it returns, and says so in the log.
//
//   - start() IS THE ONLY THING THAT KEYS IT, and it keys it LAST. The port,
//     the LO frequency, the rate, the DDS shutdown and the buffer all happen
//     while the board is still at maximum attenuation; the requested
//     attenuation is written after every one of them, immediately before the
//     writer thread is spawned. Every step before that one is inaudible on
//     the air, which is what makes a failure halfway through start() safe.
//
//   - stop() SILENCES BEFORE IT TIDIES, in that order: attenuation to
//     maximum first, then the LO down, then the buffer. The reverse order
//     would leave a keyed board transmitting whatever the DAC held for as
//     long as the tidying took.
//
//   - ~PlutoTx() CALLS IT. A caller who forgets cannot leave a radio keyed.
//
// WHERE THE SILENCING RUNS, and why it is not on the caller's thread. The two
// quieting writes are the LAST ACT OF THE WRITER THREAD, on the writer's own
// connection, rather than work stop() does itself after the join. Three
// reasons, and the third is the one that decided it:
//   1. the writer is the thread that knows the board is still answering;
//   2. it is already between WRITEBUFs, so nothing has to be interleaved;
//   3. it makes stop() ONE bounded wait instead of a join plus two network
//      round trips, which is what keeps the transmit column of
//      tests/test_shutdown_budget.cpp to kWriterJoinWait alone.
// An ABANDONED writer (see below) is not killed: it keeps trying to silence
// the board on its own leaked connection, so a board that comes back inside
// its own bounds is still told to shut up - it simply happens after stop()
// has returned rather than before.
//
// ============================================================================
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT
// ============================================================================
//
// One writer thread pulls from a ring, converts to the board's sample format
// and issues WRITEBUFs; write() fills the ring from the transmitter's thread.
// The join is BOUNDED (kWriterJoinWait) and a writer that has not come back
// by then is ABANDONED rather than waited for - the same trade PlutoSource
// and HackRfSource make for their readers, and for the same reason: a hang on
// the GUI thread is worse than a leak. So everything the writer touches - the
// run flag, the stream client, the ring, the error slot - lives in a
// WriterLink behind a shared_ptr the thread captures BY VALUE, and the
// abandoning path deliberately leaks the stream client rather than destroying
// a socket a thread is still inside.
//
// TWO CONNECTIONS, AND THEY ARE NOT THE RECEIVER'S TWO. A Pluto driven for
// both directions at once therefore holds four sockets to one daemon. That is
// what libiio's own network backend does per context and the daemon is built
// for it (one thread per connection, ops.c); the alternative - sharing the
// receiver's control connection - would mean a retune and a transmit
// attenuation change interleaving their replies on one socket, which is
// exactly the fault the receiver opened a second connection to avoid.
//
// WHAT THIS DOES NOT DO, stated so it is not mistaken for an oversight:
// setSampleRateHz writes the AD9361's TRANSMIT sampling frequency, and on an
// AD9361 that is the same clock the receiver runs on - writing it moves the
// receive rate too. So the transmitter READS the rate and resamples to it
// rather than setting it (see core/transmitter.hpp), and this setter exists
// for the case where somebody deliberately wants the board's clock moved.
//
// NOTHING HERE HAS BEEN RUN AGAINST A PLUTO. There is none on this bench, and
// nothing on it may transmit. What is proven is the conversation - every byte
// of it - against a fake daemon written from the same grammar the real one
// parses; tests/test_pluto_tx.cpp says what that can and cannot establish.
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
#include "source/iiod_client.hpp"
#include "source/tx_sink.hpp"

namespace cascade::source {

// --- the buffer geometry ----------------------------------------------------

// Samples per transmit buffer, and how many of them the daemon keeps. Smaller
// than the receive side's 16384 on purpose: a transmit buffer is LATENCY
// BETWEEN THE KEY AND THE AIR, not throughput. 4096 samples is 1.6 ms at the
// slowest rate a stock Pluto takes (2.083 MS/s) and 67 us at its fastest, so
// the delay between releasing a PTT and the board running out of modulation
// is under two milliseconds at either end. Two buffers is the least that
// keeps the DAC fed while the next one is being filled.
constexpr std::size_t kTxSamplesPerBuffer = 4096;
constexpr long kTxBuffersCount = 2;

class PlutoTx : public IqSink {
public:
    // --- the bounded waits --------------------------------------------------
    //
    // Named rather than written at their call sites because
    // tests/test_shutdown_budget.cpp discovers `constexpr std::chrono`
    // constants under src/ and refuses to go green until each is classified
    // in its kKnownWaits table. Class members and not namespace-scope
    // constants for the reason pluto_source.hpp gives: several headers in
    // this directory already declare names like these at cascade::source
    // scope, and app_window.cpp includes more than one of them.

    // The bound on joining the writer thread, and the WHOLE of the transmit
    // column of the shutdown budget. The writer's job on the way out is two
    // one-line attribute writes, which on a board that is answering cost well
    // under a millisecond; a board that needs longer than this to answer two
    // of them is a board that has gone, and abandoning a thread that keeps
    // trying is strictly better than holding the window shut while it does.
    //
    // DELIBERATELY SHORTER THAN iiod::kReplyWait's 2000 ms, which is the
    // opposite of the choice PlutoSource::kReaderJoinWait makes. The reader's
    // longest legitimate stall IS one socket receive, so a shorter join there
    // would abandon a thread that was coming back. The writer's is not: it is
    // only ever waiting for the daemon to acknowledge something it asked for
    // a moment ago on a connection that was working, and this is a teardown
    // wait with a keyed radio on the end of it.
    static constexpr std::chrono::milliseconds kWriterJoinWait{1500};

    // How long write() will wait for room in the ring before returning short.
    // Spent on the transmitter's own thread, which is also the thread that
    // notices the PTT has been released - so it is deliberately a fraction of
    // the shortest key-up a hand can produce.
    static constexpr std::chrono::milliseconds kWriteWait{20};

    PlutoTx() = default;
    ~PlutoTx() override;

    // Owns two connections and a thread; copying any of them would be a
    // double-close, a double-join, or a radio nobody can silence.
    PlutoTx(const PlutoTx&) = delete;
    PlutoTx& operator=(const PlutoTx&) = delete;

    // --- opening ------------------------------------------------------------

    // Connects (bounded by iiod::kConnectWait), asks VERSION, PRINTs the
    // context and finds the transmit half of it, reads the board's own limits
    // - and LEAVES THE BOARD QUIET (see the file header). Nothing is
    // transmitted until start(). The args string is the same one
    // PlutoSource::open takes, parsed by the same parsePlutoUri.
    bool open(const std::string& args);

    // Stops transmitting and drops both connections. Idempotent, safe on a
    // never-opened instance, and safe from the destructor. lastError()
    // survives it.
    void close();

    bool isOpen() const { return openMirror_.load(std::memory_order_relaxed); }

    // --- IqSink -------------------------------------------------------------

    bool start() override;
    void stop() override;
    bool running() const override { return running_.load(std::memory_order_relaxed); }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }
    bool setCenterFrequencyHz(double hz) override;

    bool frequencyRangeHz(double& loHz, double& hiHz) const override;
    bool sampleRateRangeHz(double& loHz, double& hiHz) const override;

    // The TX attenuation, in the board's own negative decibels. See the
    // warning in tx_sink.hpp: the "more power" direction here is UP TOWARDS
    // ZERO, and out-of-range values are CLAMPED, which for this control means
    // a stray large number lands on the QUIET end rather than on full output
    // - clampTxGainDb below is where that is decided, and it is tested.
    bool gainRangeDb(double& loDb, double& hiDb) const override;
    double gainDb() const override { return gainDb_.load(std::memory_order_relaxed); }
    bool setGainDb(double db) override;

    std::size_t write(const std::complex<float>* samples, std::size_t n) override;

    bool faulted() const override;
    const char* name() const override;
    const char* lastError() const override;

    // --- what open() read off the board -------------------------------------
    std::string hardwareModel() const;
    std::string phyDeviceName() const;
    std::string dacDeviceName() const;
    // The phy channel the TX LO lives on ("altvoltage1") and the one the
    // attenuation and the port live on ("voltage0", an OUTPUT channel).
    std::string txLoChannel() const;
    std::string txChannel() const;

    // Buffers the writer had to pad with silence because the ring did not
    // hold a whole one in time - the modulator fell behind the radio. Counted
    // rather than tolerated silently: a transmission with gaps in it and no
    // number anywhere is how a slow machine looks exactly like a broken
    // board. (The opposite case, the modulator getting AHEAD of the radio, is
    // visible to the caller already: write() returns short.)
    std::uint64_t underrunBuffers() const;

    // Writer threads this PROCESS has abandoned because they did not come
    // back within kWriterJoinWait. 0 on every healthy path; the delta is what
    // a test asserts, because elapsed time alone still passes when the bound
    // is deleted.
    static unsigned long long writersAbandoned();

private:
    struct WriterLink {
        explicit WriterLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // The stream connection. NOT owned here: the PlutoTx owns it, except
        // on the abandoning path, which deliberately leaks it so a stranded
        // writer still has a live object to be inside - and still has
        // something to silence the board through.
        iiod::Client* stream = nullptr;
        std::string dacDevice;
        std::string phyDevice;
        std::string txChannel;
        std::string txLoChannel;
        bool haveLoPowerdown = false;
        double quietDb = 0.0;  // the board's own maximum attenuation
        std::size_t bufferSamples = 0;
        iiod::SampleFormat format;

        cascade::dsp::SpscRing<std::complex<float>> ring;

        // write() parks here when the ring is full; the writer signals after
        // every buffer it takes. `exited` is the writer's LAST act and the
        // thing the bounded join waits for: a thread that has not set it has
        // not left, and "the flag is still clear" is evidence a timing
        // measurement on its own cannot produce.
        std::mutex waitMutex;
        std::condition_variable waitCv;
        bool exited = false;

        // Set by the writer once the two quieting writes have been accepted.
        // Read by the tests, and by nothing in the product: it is the only
        // way to tell "the board was told to shut up" from "the board was
        // asked to shut up and did not answer", and those are different
        // things to report.
        std::atomic<bool> silenced{false};

        mutable std::mutex errorMutex;
        std::string lastError;
        bool faulted = false;

        std::atomic<std::uint64_t> underruns{0};
    };

    // NEVER REASSIGNED, hence const: an abandoned writer holds its own copy
    // of this pointer, so a sink that swapped in a fresh link could have a
    // new connection behind a thread still pumping the old one.
    const std::shared_ptr<WriterLink> link_ = std::make_shared<WriterLink>(kRingCapacitySamples());

    // Eight whole buffers, rounded to the power of two SpscRing wants. At
    // 2.5 MS/s that is 13 ms of modulation in flight, which is the most a
    // released PTT can cost; deeper would only buy throughput this path does
    // not need.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 15; }

    // The *Locked helpers assume devMutex_ is held.
    bool writeChanAttrLocked(const std::string& device, bool output, const std::string& channel,
                             const char* attr, const std::string& value, const char* what);
    bool readChanAttrLocked(const std::string& device, bool output, const std::string& channel,
                            const char* attr, std::string& value, const char* what);
    // Attenuation to the board's own maximum and the TX LO down, in that
    // order, on the CONTROL connection. Used by open() and by a start() that
    // failed part way; the ordinary stop() path runs the same two writes on
    // the writer's connection instead (see the file header).
    bool quietenLocked(const char* what);
    bool programGainLocked(double db);

    bool startWritingLocked();
    void stopWritingLocked();

    // THE WRITER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned writer outlives the PlutoTx; a member function reaching for a
    // member of a destroyed object is precisely the defect the link was
    // introduced to prevent.
    static void writerThreadBody(std::shared_ptr<WriterLink> link);
    static void silenceOn(WriterLink& link);
    static void setErrorOn(WriterLink& link, std::string msg);
    static void noteFaultOn(WriterLink& link, const char* what, const std::string& detail);
    void setError(std::string msg);
    void clearError();
    void noteFault(const char* what, const std::string& detail);
    void setName(std::string n);

    mutable std::mutex devMutex_;

    std::unique_ptr<iiod::Client> control_;
    // Owned, except after an abandonment - see the file header.
    std::unique_ptr<iiod::Client> stream_;
    std::thread writer_;

    std::string host_;
    std::uint16_t port_ = iiod::kDefaultPort;
    std::string phyDevice_;
    std::string dacDevice_;
    std::string txLoChannel_;  // "altvoltage1"
    std::string txChannel_;    // "voltage0", an OUTPUT channel on the phy
    std::size_t dacChannels_ = 0;
    std::vector<std::size_t> dacEnabled_;
    // Every OUTPUT channel on the DAC device that carries a `raw` attribute
    // and no scan element: the AD9361's internal DDS tone generators, which
    // have to be written 0 or the DAC plays them and ignores the buffer.
    std::vector<std::string> ddsChannels_;
    iiod::SampleFormat format_;
    std::string hwModel_;
    bool haveLoPowerdown_ = false;
    bool haveTxPort_ = false;

    bool haveLoRange_ = false;
    iiod::Range loRange_;
    bool haveRateRange_ = false;
    iiod::Range rateRange_;
    bool haveBwRange_ = false;
    iiod::Range bwRange_;
    bool haveGainRange_ = false;
    iiod::Range gainRange_;

    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<double> gainDb_{0.0};

    mutable std::mutex nameMutex_;
    std::string name_ = "ADALM-Pluto transmit: (not connected)";
};

// The attenuation a request lands on, given the board's published range.
//
// A FREE FUNCTION, AND TESTED ON ITS OWN, because it is the one piece of
// arithmetic in this file where getting the direction wrong is a licence
// violation rather than a bug. An AD9361 publishes something like
// "[-89.750000 0.250000 0.000000]" - min is the QUIETEST and max is FULL
// OUTPUT - so the ordinary "clamp into range" that every receive gain uses
// would turn a hand-edited 40 into 0 dB, which is maximum power. It still
// clamps, because a control that refuses is a control that leaves the board
// wherever it was; what it does not do is let the clamp be written by
// accident. NaN lands on the quiet end for the same reason packSample writes
// a NaN sample as zero.
double clampTxGainDb(double requested, const iiod::Range& range);

}  // namespace cascade::source
