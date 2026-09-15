// pluto_source.hpp - FoxSDR's own ADALM-Pluto driver: a DeviceSource that
// speaks the IIOD network protocol in src/source/iiod_client.hpp to the
// daemon on the board, with no libiio, no libad9361 and no SoapySDR module
// anywhere in the path.
//
// THE PLUTO IS NOT A USB RADIO, from here. It presents a USB Ethernet gadget
// and runs an `iiod` daemon on TCP port 30431, so everything the other native
// drivers do with control transfers this one does with one-line text
// commands, and everything they do with bulk endpoints this one does with
// READBUF. The consequences run all the way through the file:
//
//   - ENUMERATION CANNOT WALK A NETWORK. A USB driver asks SetupAPI what is
//     plugged in; there is no equivalent for "is there a Pluto somewhere on
//     the network", and probing to find out would break the rule that
//     enumeration never opens a device. So enumeratePluto() OFFERS the two
//     addresses a Pluto answers at out of the box and contacts neither: the
//     rows say "(not yet contacted)" because that is the truth, and the user
//     can type any other address instead.
//
//   - TWO CONNECTIONS, not one. libiio's network backend opens a second
//     socket for the sample stream, and for the same reason we do: a retune
//     is a WRITE that must not queue behind a READBUF the daemon has not
//     answered yet. The control connection belongs to whoever holds
//     devMutex_; the stream connection belongs to the reader thread while it
//     runs and to nobody in between.
//
//   - THE LIMITS COME OFF THE BOARD. A Pluto may be a stock AD9363 (325 MHz
//     to 3.8 GHz) or one with the AD9364 unlock applied (70 MHz to 6 GHz),
//     and the same firmware serves both. Publishing a table would therefore
//     be wrong for half the users in one direction or the other, so the LO
//     range, the sample-rate range and the bandwidth range are all read from
//     the *_available attributes at open and reported from those. When the
//     board does not publish one, frequencyRangeHz() answers false - "not
//     known" - rather than inventing a number.
//
// THE THREAD, AND WHY IT OUTLIVES THIS OBJECT. One reader thread issues
// READBUFs on the stream connection, converts the samples and writes them
// into a ring; read() drains the ring on the pipeline's source thread. The
// join is BOUNDED (kReaderJoinWait) and a reader that has not come back by
// then is ABANDONED rather than waited for, exactly as HackRfSource abandons
// a wedged reader, because a hang on the GUI thread is worse than a leak. An
// abandoned reader must still have somewhere valid to run, so everything it
// touches - the run flag, the stream client, the ring, the error slot, the
// health tally - lives in a ReaderLink behind a shared_ptr the thread
// captures BY VALUE, and the abandoning path deliberately leaks the stream
// client rather than destroying a socket a thread is still inside.
//
// WHY kReaderJoinWait IS LONGER HERE THAN ON THE USB DRIVERS. Their readers
// park in a 100 ms bulk read; ours parks in a socket receive bounded by
// iiod::kReplyWait, which is 2000 ms because that is a sensible bound for a
// network round trip. The join must be longer than that or EVERY stop of a
// quiet board would abandon a reader that was about to return normally. In
// practice the reader is inside a READBUF that completes in one buffer period
// - about 7 ms at 2.5 MS/s - so a healthy stop costs milliseconds; the bound
// is for the case where the board has gone.
//
// WHAT IS NOT HERE: TRANSMIT. The Pluto is a transceiver and this is a
// receiver driver. What a transmit path would need, recorded so it is not
// rediscovered from scratch: the DDS/capture device is `cf-ad9361-dds-core-
// lpc` and its channels are OUTPUTS, so the buffer is opened with a mask over
// its out voltage0/voltage1 and fed with WRITEBUF <dev> <bytes> followed by
// the raw sample bytes (ops.c rw_dev with is_write true -> receive_data,
// which answers a single "0" line before it starts reading and a byte count
// after). The TX LO is out_altvoltage1 (named TX_LO in the context XML, as
// against altvoltage0 = RX_LO), the TX gain is out_voltage0 hardwaregain and
// is an ATTENUATION in negative dB, and the TX port is out_voltage0
// rf_port_select. None of that is exercised or tested here.
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

// --- the buffer geometry --------------------------------------------------

// Samples per capture buffer, and how many of them the daemon keeps. 16384
// samples is 6.6 ms at the Pluto's slowest rate (2.083 MS/s) and 0.27 ms at
// its fastest (61.44 MS/s), which keeps one READBUF comfortably inside the
// socket's receive bound at either end. Four buffers is what iiod defaults to
// and is enough that the board is never waiting on us between reads.
constexpr std::size_t kSamplesPerBuffer = 16384;
constexpr long kBuffersCount = 4;

// --- enumeration ----------------------------------------------------------

// The addresses a Pluto answers at, as rows the Source section can show
// WITHOUT ANY NETWORK TRAFFIC AT ALL (see the file header). 192.168.2.1 is
// what the USB Ethernet gadget serves out of the box; pluto.local is the mDNS
// name the board advertises, which resolves only if the machine has a
// responder for it. Both rows say they have not been contacted, because a
// label claiming a radio is present when nothing has asked it is a lie the
// user finds out about one click later.
std::vector<NativeDeviceInfo> enumeratePluto();

// The host and port an args string names. "uri=ip:192.168.2.1" is libiio's
// own URI spelling and is what the enumeration rows carry;
// "uri=ip:pluto.local:30431" adds a port, and a bare "host=..." is accepted
// too. An empty args string means the default address, so a user who picks
// "ADALM-Pluto" with nothing typed gets the board on the USB cable.
bool parsePlutoUri(const std::string& args, std::string& host, std::uint16_t& port,
                   std::string& error);

// --- the driver -----------------------------------------------------------

class PlutoSource : public DeviceSource {
public:
    // --- the bounded waits ------------------------------------------------
    //
    // Named rather than written at their call sites because
    // tests/test_shutdown_budget.cpp discovers `constexpr std::chrono`
    // constants under src/ and refuses to go green until each is classified
    // in its kKnownWaits table. The connect and reply bounds live next door
    // in iiod_client.hpp; these three are the driver's own.
    //
    // CLASS MEMBERS AND NOT NAMESPACE-SCOPE CONSTANTS, for the reason the
    // Airspy and RX888 headers give: hackrf_source.hpp already declares
    // kReaderJoinWait, kReadWait and kStreamHealthWindow at cascade::source
    // scope, so a translation unit that includes both - app_window.cpp does,
    // from 0.93.0, because the Source section can now open either - would not
    // compile. Found exactly that way, and the fix is the established one
    // rather than a rename, because the names ARE the right names.

    // The bound on joining the reader thread (see the file header). Longer
    // than iiod::kReplyWait on purpose: the reader's longest legitimate stall
    // is one socket receive, and a join shorter than that would abandon a
    // thread that was coming back.
    static constexpr std::chrono::milliseconds kReaderJoinWait{2500};

    // How long read() waits for the reader to put something in the ring
    // before returning the IqSource contract's "nothing yet, retry" zero. The
    // pipeline's self-paced loop then backs off a millisecond and asks again.
    static constexpr std::chrono::milliseconds kReadWait{20};

    // The stream-health window, matching SoapySource::kStreamHealthWindow.
    // Not a wait: nothing sleeps or blocks on it. It is how much streaming
    // the reader tallies before it writes one "source: stream health ..."
    // line.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    PlutoSource() = default;
    ~PlutoSource() override;

    // Owns two connections and a thread; copying any of them would be a
    // double-close or a double-join.
    PlutoSource(const PlutoSource&) = delete;
    PlutoSource& operator=(const PlutoSource&) = delete;

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "pluto"; }

    // Connects (bounded by iiod::kConnectWait), asks VERSION so a wrong
    // service on the right port fails as "not an IIO daemon" rather than as
    // gibberish, PRINTs the context and parses it, then reads the board's own
    // limits and current state off the two devices it found. Nothing is
    // streamed until start().
    bool open(const std::string& args) override;

    // Stops the stream and drops both connections. Idempotent, safe on a
    // never-opened instance, and safe from the destructor. lastError()
    // survives it.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // One gain, "RX", in real decibels, with the range read from the board's
    // own hardwaregain_available. Out-of-range values are CLAMPED and
    // gainDb() reports what was actually programmed - the DeviceSource
    // contract.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // The AD9361 has a real AGC. "On" is slow_attack, which is the one that
    // suits a receiver listening to a steady signal; fast_attack chases
    // bursts and makes a waterfall breathe. "Off" is manual.
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return autoGain_.load(std::memory_order_relaxed); }

    // One receive port is brought out on the board, the balanced A input.
    // The AD9361 has more and the driver publishes them in
    // rf_port_select_available, but they go nowhere on a Pluto, and offering
    // a choice that silences the radio is worse than offering none.
    std::vector<std::string> antennas() const override { return {"A_BALANCED"}; }
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override { return "A_BALANCED"; }

    // A menu built from the board's own sampling_frequency_available range -
    // the AD9361 takes any rate in it, not a table of them - always including
    // the two ends so the panel can offer exactly what the board will accept.
    std::vector<double> supportedSampleRatesHz() const override;

    // From the RX LO's frequency_available. False when the board did not
    // publish one: "not known" is an answer, a made-up range is not.
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // SET BUFFERS_COUNT, then OPEN, then the reader thread - in that order,
    // because the daemon reads the buffer count only when it creates the
    // buffer, which happens inside OPEN (ops.c create_buf_and_blocks). Sent
    // afterwards it would succeed, change nothing, and leave the stream
    // running at the default depth with no sign anything had been ignored.
    bool start() override;

    // Reader joined (bounded, see the file header), then the stream
    // connection dropped - which is what closes the capture device, because
    // the daemon closes every device a connection held when its read returns
    // 0 (ops.c ascii_interpreter). No CLOSE command is sent: see
    // stopStreamingLocked for what that would cost the shutdown budget.
    // Idempotent, safe before open.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Coerced to what the board says it takes: above its maximum is clamped
    // DOWN, and below its minimum is REFUSED with a sentence rather than
    // quietly raised.
    //
    // THE FIR RULE, and why the refusal is honest rather than lazy. Below
    // about 2.083 MS/s the AD9361 cannot decimate far enough on its own and
    // needs a FIR filter loaded into it through filter_fir_config - which is
    // why that is exactly the minimum a stock Pluto reports in
    // sampling_frequency_available. This driver does not generate or load FIR
    // taps, so it refuses below whatever minimum the board reports and says
    // which attribute would have to be written to go lower. A board that
    // already has a FIR loaded reports a lower minimum, and this driver will
    // honour it, because the number comes from the board and not from here.
    //
    // ON A RUNNING STREAM the change is made with the radio QUIET: the reader
    // stopped and the stream connection dropped - which closes the capture
    // device - before the clock underneath it moves, then a fresh connection
    // opened. running() reads true throughout on the success
    // path, which is the shape 0.89.0 had to give the Soapy path after a live
    // sample-rate change killed the process on a driver's own reader thread.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live: one WRITE on the control connection, no stream interruption.
    // Refused (with a reason) outside the range the board published, rather
    // than clamped, because a tune that silently lands somewhere else is
    // worse than one that does not happen.
    bool setCenterFrequencyHz(double hz) override;

    // Drains the reader's ring. Blocks at most kReadWait when it is empty and
    // then returns 0 - the self-paced contract's "nothing yet, retry".
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // True once the connection has failed or the board has gone. The
    // pipeline's source loop polls it and stops with lastError().
    bool faulted() const override;

    const char* name() const override;
    const char* lastError() const override;

    // --- what open() read off the board ----------------------------------
    // Empty / zero before a successful open. Kept because "which board is
    // this, what firmware is on it, and is it unlocked" is the first question
    // any Pluto problem report needs answered.
    std::string hardwareModel() const;
    std::string firmwareVersion() const;
    std::string daemonVersion() const;
    std::string serialNumber() const;
    // The crystal's measured correction in Hz, as the board reports it in
    // ad9361-phy's xo_correction. A Pluto's XO is a plain 40 MHz part and
    // this is how far off it was found to be; it is worth having in a report
    // when somebody says every signal is 2 kHz low.
    double xoCorrectionHz() const;
    // The path rates line (BBPLL / ADC / R2 / R1 / RF / RXSAMP), verbatim.
    std::string rxPathRates() const;
    // True when the LO range the board published reaches past what a stock
    // AD9363 covers - i.e. the AD9364 unlock has been applied. DERIVED from
    // the published range, not from a firmware version or a guess.
    bool ad9364Unlocked() const;

    // The name of the phy device and of the capture device as they were found
    // in the context, for the log and for the tests.
    std::string phyDeviceName() const;
    std::string captureDeviceName() const;

    // --- stream health ---------------------------------------------------
    //
    // Same tally and the same one-line format SoapySource writes: a line
    // always for the first window after a start, so a healthy radio leaves
    // one proving it, and after that only for a window with something to
    // report.
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

    // The line for the window so far, and the window starts again. Empty when
    // nothing has been read since the last line.
    std::string streamHealthLine();

    // Tests only: shorten the window so the reader's own reporting can be
    // seen without waiting a minute for it.
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // Buffers the reader has had to drop because the ring was full - the host
    // fell behind, not the radio.
    std::uint64_t droppedBuffers() const;

    // Reader threads this PROCESS has abandoned because they did not come
    // back within kReaderJoinWait. 0 on every healthy path; the delta is what
    // a test asserts, because elapsed time alone still passes when the bound
    // is deleted.
    static unsigned long long readersAbandoned();

private:
    // Everything the reader thread touches, in one object behind a shared_ptr
    // it captures BY VALUE - see the file header.
    struct ReaderLink {
        explicit ReaderLink(std::size_t ringCapacity) : ring(ringCapacity) {}

        std::atomic<bool> run{false};

        // The stream connection. NOT owned here: the PlutoSource owns it,
        // except on the abandoning path, which deliberately leaks it so a
        // stranded reader still has a live object to be inside.
        iiod::Client* stream = nullptr;
        std::string captureDevice;
        std::size_t bufferBytes = 0;
        iiod::SampleFormat format;

        cascade::dsp::SpscRing<std::complex<float>> ring;

        // read() parks here when the ring is empty; the reader signals after
        // every buffer it writes. `exited` is the reader's LAST act and the
        // thing the bounded join waits for: a thread that has not set it has
        // not left, and "the flag is still clear" is evidence a timing
        // measurement on its own cannot produce.
        std::mutex waitMutex;
        std::condition_variable waitCv;
        bool exited = false;

        // The error slot: written by the reader, read by the GUI and by the
        // pipeline's source loop. A std::string written on one thread and
        // read on another is UB, not a stale value.
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

    // NEVER REASSIGNED, hence const: an abandoned reader holds its own copy
    // of this pointer, so a source that swapped in a fresh link could have a
    // new connection behind a thread still pumping the old one.
    const std::shared_ptr<ReaderLink> link_ = std::make_shared<ReaderLink>(kRingCapacitySamples());

    // The ring holds eight whole buffers, rounded up to the power of two
    // SpscRing requires. At 61.44 MS/s that is 2 ms of signal and at
    // 2.5 MS/s it is 52 ms, against a pipeline that asks for 10 ms chunks.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 18; }

    // The *Locked helpers assume devMutex_ is held.
    bool readAttrLocked(const std::string& device, const char* attr, std::string& value,
                        const char* what);
    bool readChanAttrLocked(const std::string& device, bool output, const std::string& channel,
                            const char* attr, std::string& value, const char* what);
    bool writeChanAttrLocked(const std::string& device, bool output, const std::string& channel,
                             const char* attr, const std::string& value, const char* what);
    bool programFrequencyLocked(double hz);
    bool programSampleRateLocked(double hz);
    bool programGainLocked(double db);
    bool programGainModeLocked(bool automatic);

    // SET BUFFERS_COUNT, OPEN, spawn the reader. Assumes devMutex_ held, the
    // board open and not already streaming.
    bool startStreamingLocked();
    // Signal, bounded join, drop the stream connection. Idempotent; assumes
    // devMutex_ held.
    void stopStreamingLocked();

    // THE READER'S OWN HELPERS ARE STATIC AND TAKE THE LINK, not `this`. An
    // abandoned reader outlives the PlutoSource; a member function reaching
    // for a member of a destroyed object is precisely the defect the link was
    // introduced to prevent, and making these static is what stops the
    // compiler from letting one be written by accident.
    static void readerThreadBody(std::shared_ptr<ReaderLink> link);
    static void noteRead(ReaderLink& link, bool ok, std::size_t samples, bool dropped);
    static std::string healthLineLocked(ReaderLink& link);  // link.healthMutex held

    static void setErrorOn(ReaderLink& link, std::string msg);
    static void noteFaultOn(ReaderLink& link, const char* what, const std::string& detail);
    void setError(std::string msg);
    void clearError();
    void noteFault(const char* what, const std::string& detail);

    void setName(std::string n);

    // Serialises control commands against each other and against
    // start/stop/open/close. The reader thread does not take it: it only ever
    // touches the STREAM connection, which nothing else uses while it runs.
    mutable std::mutex devMutex_;

    std::unique_ptr<iiod::Client> control_;
    // Owned, except after an abandonment - see the file header.
    std::unique_ptr<iiod::Client> stream_;
    std::thread reader_;

    // What the context said. Read once at open under devMutex_.
    std::string host_;
    std::uint16_t port_ = iiod::kDefaultPort;
    std::string phyDevice_;
    std::string captureDevice_;
    std::string rxLoChannel_;   // "altvoltage0"
    std::string rxChannel_;     // "voltage0" on the phy, where the gains live
    std::size_t captureChannels_ = 0;
    std::vector<std::size_t> captureEnabled_;
    iiod::SampleFormat format_;
    std::string hwModel_;
    std::string hwSerial_;
    std::string fwVersion_;
    std::string daemonVersion_;
    std::string rxPathRates_;
    double xoCorrectionHz_ = 0.0;

    // The board's own limits, and whether it published each one.
    bool haveLoRange_ = false;
    iiod::Range loRange_;
    bool haveRateRange_ = false;
    iiod::Range rateRange_;
    bool haveBwRange_ = false;
    iiod::Range bwRange_;
    bool haveGainRange_ = false;
    iiod::Range gainRange_;
    std::vector<std::string> gainModes_;

    // Lock-free mirrors, so per-frame GUI readouts never wait behind a
    // command in flight.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<double> gainDb_{0.0};
    std::atomic<bool> autoGain_{false};

    mutable std::mutex nameMutex_;
    std::string name_ = "ADALM-Pluto: (not connected)";
};

}  // namespace cascade::source
