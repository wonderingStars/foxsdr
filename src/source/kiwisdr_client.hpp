// kiwisdr_client.hpp - a LISTEN-ONLY client for one KiwiSDR receiver: one
// WebSocket to /<timestamp>/SND through the vendored cpp-httplib client, a
// reader thread that decodes the audio frames into a lock-free ring of 16-bit
// samples, a keep-alive about once a second, tune and passband commands, and a
// status the caller can read. STAND-ALONE: it is not a DeviceSource, it is not
// wired into the engine, the settings or the screen. A later lane wraps it
// (docs/feature-records/kiwi-client.md, WIRING HAND-OFF).
//
// WHAT IT IS NOT: no waterfall (W/F frames are counted and dropped), no I/Q
// mode (buildTune refuses "iq"), no transmit, no secure WebSocket (ws:// only:
// the vendored httplib is built without OpenSSL on Windows), no extensions.
//
// POLITENESS IS PART OF THE CLIENT, not a policy the caller has to remember
// (facts report section 3, "Terms to honour"):
//  * It sends ident_user=FoxSDR (the name is an option, the default is FoxSDR).
//  * It opens ONE connection per object. It NEVER reconnects by itself: a
//    dropped, refused or stalled connection is reported and stays reported;
//    the caller makes a new object if the user presses Open again.
//  * It NEVER follows a redirect: the target is reported (status().redirect)
//    and the connection is dropped; the caller decides.
//  * It stops AT ONCE on too_busy, badp (any code but "accepted") and down:
//    the keep-alive stops, the socket is shut, the state is sticky.
//  * No scanning, no second connection, no camping: nothing in this class
//    can start another connection.
//
// PROVENANCE: the protocol is the outline in kiwisdr_protocol.hpp (clean room).
// Nothing here has ever spoken to a real KiwiSDR.
//
// THREADS. Two threads per open client: a reader (blocked in the WebSocket
// read, decodes, pushes to the ring) and a keep-alive timer. Both hold the
// shared link by value, so an abandoned thread never touches freed memory
// (the rtl_tcp pattern: bounded 2.5 s join, then abandon and count). Neither
// allocates per frame once running except the std::string httplib hands the
// reader for each message. Samples reach the caller through read(); every
// other query is an atomic or a short mutex.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cascade::source {

class KiwiSdrClient {
public:
    // How long close() waits for the threads before abandoning them. Matches
    // RtlTcpSource::kReaderJoinWait.
    static constexpr std::chrono::milliseconds kJoinWait{2500};

    // The keep-alive cadence ("about once a second").
    static constexpr std::chrono::milliseconds kKeepAliveInterval{1000};

    // The default bound on connect, and the default bound on silence: a
    // connection that delivers nothing for this long is reported as dropped.
    static constexpr std::chrono::milliseconds kConnectWait{5000};
    static constexpr std::chrono::milliseconds kStallWait{15000};

    // The ring holds this many 16-bit samples (about 5.5 s at 12 kHz); a
    // power of two. A consumer that falls further behind loses the newest
    // samples, which are counted (droppedSamples()).
    static constexpr std::size_t kRingSamples = 65536;

    enum class State {
        Idle,             // nothing opened
        Connecting,       // socket / handshake / waiting for the first audio frame
        Playing,          // at least one audio frame has arrived
        RefusedBusy,      // too_busy
        RefusedPassword,  // badp with a code that is not "accepted"
        ServerDown,       // down
        Redirected,       // redirect (never followed)
        ConnectFailed,    // no TCP connection / no WebSocket upgrade
        Dropped,          // the connection ended or stalled after it was made
        Malformed,        // a frame the parser refused
        Closed,           // close() by the caller
    };

    struct Options {
        std::string host = "127.0.0.1";
        std::uint16_t port = 8073;
        std::string password;                       // empty: none
        std::string identUser = "FoxSDR";
        bool adpcm = true;                          // ask for IMA ADPCM (less bandwidth)
        std::chrono::milliseconds connectWait = kConnectWait;
        std::chrono::milliseconds stallWait = kStallWait;
        std::chrono::milliseconds keepAliveInterval = kKeepAliveInterval;
        std::uint64_t timestamp = 0;                // path component; 0: the current Unix time
    };

    struct Status {
        State state = State::Idle;
        int code = 0;                // too_busy / badp / down value (-1: not a number)
        std::string redirect;        // the target, for Redirected
        std::string detail;          // one sentence for a log or the screen
    };

    KiwiSdrClient();
    ~KiwiSdrClient();
    KiwiSdrClient(const KiwiSdrClient&) = delete;
    KiwiSdrClient& operator=(const KiwiSdrClient&) = delete;

    // Connects (bounded), upgrades to a WebSocket and sends the opening
    // commands in this order: auth, ident_user, AR OK, compression. Tune is
    // NOT sent here: call tune() next. True when the socket is up and the
    // threads run - NOT when audio flows: watch status(). False when it could
    // not connect; status() is ConnectFailed. A client opens at most once;
    // a second open() returns false.
    bool open(const Options& options);

    // Sends the mode / passband / frequency command now. False when nothing
    // is open, the connection is finished, or the arguments are refused (a
    // mode that is not 1-8 lower-case letters or digits, "iq", a frequency
    // that is negative or not finite). The latest accepted values are also
    // reported by mode() / frequencyHz().
    bool tune(const std::string& mode, int lowCutHz, int highCutHz, double frequencyHz);

    // Ends the connection: shuts the socket (which wakes the reader at once),
    // waits at most kJoinWait for both threads and abandons any that do not
    // return. Idempotent, safe from any thread, safe on a never-opened object
    // and from the destructor; it keeps a terminal state that was already
    // reported (RefusedBusy stays RefusedBusy) and otherwise sets Closed. No
    // WebSocket close frame is sent: the TCP connection is shut (see the
    // record, ASSUMPTIONS).
    void close();

    // Copies up to n decoded samples (mono, 16-bit, audioRateHz() per second)
    // into dst and returns how many. waits at most `wait` for the first one.
    // One consumer thread only.
    std::size_t read(std::int16_t* dst, std::size_t n,
                     std::chrono::milliseconds wait = std::chrono::milliseconds(0));

    Status status() const;
    State state() const;

    // The audio rate samples are delivered at (kiwi::assumed::kAudioRateHz).
    static int audioRateHz();

    // The newest S-meter word and its RSSI in dBm; haveSmeter() is false until
    // the first audio frame.
    bool haveSmeter() const;
    std::uint16_t smeterRaw() const;
    double rssiDbm() const;

    // Sequence bookkeeping: gaps are the number of times the sequence jumped
    // (forward or back); missedFrames is how many frames a forward jump
    // skipped. Frames and samples are totals since open.
    std::uint64_t sequenceGaps() const;
    std::uint64_t missedFrames() const;
    std::uint64_t audioFrames() const;
    std::uint64_t samplesDelivered() const;
    std::uint64_t droppedSamples() const;  // refused by a full ring
    std::uint64_t ignoredFrames() const;   // W/F, EXT, unknown tags, plain MSG
    std::uint64_t keepAlivesSent() const;

    std::string mode() const;
    double frequencyHz() const;

    // Threads close() had to abandon, process-wide (a health counter for the
    // wrapper; the engine raises its ReaderLeak notice from this).
    static std::uint64_t threadsAbandoned();

    struct Link;  // defined in the .cpp; public only so the thread bodies can name it

private:
    std::shared_ptr<Link> linkSnapshot() const;

    mutable std::mutex linkMutex_;  // guards link_ (open() and close() may race)
    std::shared_ptr<Link> link_;
    std::mutex threadMutex_;        // guards reader_, keeper_ and opened_
    std::thread reader_;
    std::thread keeper_;
    bool opened_ = false;
};

}  // namespace cascade::source
