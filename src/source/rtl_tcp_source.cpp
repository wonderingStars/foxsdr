// The rtl_tcp client itself. See rtl_tcp_source.hpp for the wire format, its
// provenance, the state rule, the one-connection design and the threading argument.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rtl_tcp_source.hpp"

#include "core/diag_log.hpp"
#include "core/ppm_correction.hpp"
#include "source/rtl2832u.hpp"
#include "source/tuner_r82xx.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cascade::source {

namespace {

// Abandoned reader threads, process-wide. See readersAbandoned().
std::atomic<unsigned long long> g_readersAbandoned{0};

// The command numbers, spelled once. tests/test_rtl_tcp_source.cpp asserts the
// bytes against LITERALS, not these names, so a wrong number here cannot be
// agreed with by the test that is meant to catch it.
constexpr std::uint8_t kCmdFrequency = 0x01;
constexpr std::uint8_t kCmdSampleRate = 0x02;
constexpr std::uint8_t kCmdGainMode = 0x03;
constexpr std::uint8_t kCmdTunerGain = 0x04;
constexpr std::uint8_t kCmdFrequencyCorrection = 0x05;
constexpr std::uint8_t kCmdAgcMode = 0x08;
constexpr std::uint8_t kCmdGainByIndex = 0x0d;
constexpr std::uint8_t kCmdBiasTee = 0x0e;

constexpr std::size_t kHeaderBytes = 12;

// The tuner types the header can name.
constexpr std::uint32_t kTunerE4000 = 1;
constexpr std::uint32_t kTunerR820T = 5;
constexpr std::uint32_t kTunerR828D = 6;

// What the tuners cover, for frequencyRangeHz() - information, never a limit.
constexpr double kR82xxLoHz = 24.0e6;
constexpr double kR82xxHiHz = 1766.0e6;
constexpr double kE4000LoHz = 52.0e6;
constexpr double kE4000HiHz = 2200.0e6;

// A middling manual gain for the open, as the native driver's default (29.7 dB
// on the R82xx ladder). Full automatic pumps badly on an R820T.
constexpr double kDefaultGainDb = 30.0;

// One receive. 64 KiB is a few tens of milliseconds at the top rate.
constexpr std::size_t kRecvBytes = 65536;

// The unsigned-byte to float conversion, as a table - the same arithmetic as
// the native RTL-SDR driver (rtlsdr_source.cpp), because it is the same chip's
// output: (b - 127.5) / 128, which keeps a full-scale byte inside [-1, 1).
struct ByteToFloat {
    float v[256];
    ByteToFloat() {
        for (int i = 0; i < 256; ++i) {
            v[i] = (static_cast<float>(i) - 127.5f) * (1.0f / 128.0f);
        }
    }
};
const ByteToFloat kByteToFloat;

std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// How a host reads in a sentence: an IPv6 literal gets its brackets back.
std::string hostPortText(const std::string& host, std::uint16_t port) {
    const bool v6 = host.find(':') != std::string::npos;
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

bool equalsNoCase(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool parsePort(const std::string& text, std::uint16_t& port) {
    if (text.empty() || text.size() > 5) { return false; }
    unsigned long v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') { return false; }
        v = v * 10 + static_cast<unsigned long>(c - '0');
    }
    if (v < 1 || v > 65535) { return false; }
    port = static_cast<std::uint16_t>(v);
    return true;
}

}  // namespace

// --- addresses and names ---------------------------------------------------

bool parseRtlTcpAddress(const std::string& args, std::string& host, std::uint16_t& port,
                        std::string& error) {
    host = "127.0.0.1";
    port = kRtlTcpDefaultPort;
    error.clear();

    const std::string text = argValue(args, "rtltcp");
    if (text.empty()) { return true; }

    std::string hostPart;
    std::string portPart;
    bool havePort = false;
    if (text[0] == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string::npos) {
            error = "\"" + text + "\" is not a valid rtl_tcp address (a bracketed IPv6 address "
                    "needs its closing bracket)";
            return false;
        }
        hostPart = text.substr(1, close - 1);
        const std::string rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') {
                error = "\"" + text + "\" is not a valid rtl_tcp address (expected :port after "
                        "the bracket)";
                return false;
            }
            portPart = rest.substr(1);
            havePort = true;
        }
    } else {
        const std::size_t colons =
            static_cast<std::size_t>(std::count(text.begin(), text.end(), ':'));
        if (colons == 1) {
            const std::size_t c = text.find(':');
            hostPart = text.substr(0, c);
            portPart = text.substr(c + 1);
            havePort = true;
        } else {
            // No colon: a host. Two or more with no brackets: a bare IPv6
            // literal, which has no room for a port and gets the default.
            hostPart = text;
        }
    }
    if (havePort && !parsePort(portPart, port)) {
        error = "\"" + portPart + "\" is not a port number (1 to 65535)";
        return false;
    }
    if (!hostPart.empty()) { host = hostPart; }
    return true;
}

const char* rtlTcpTunerName(std::uint32_t tunerType) {
    switch (tunerType) {
        case 1: return "E4000";
        case 2: return "FC0012";
        case 3: return "FC0013";
        case 4: return "FC2580";
        case 5: return "R820T";
        case 6: return "R828D";
        default: return "unknown tuner";
    }
}

// --- construction ----------------------------------------------------------

RtlTcpSource::~RtlTcpSource() { closeDevice(); }

// --- the error slot --------------------------------------------------------

void RtlTcpSource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void RtlTcpSource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void RtlTcpSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void RtlTcpSource::noteFaultOn(ReaderLink& link, const char* what, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        // THE FIRST CAUSE IS KEPT. Once a connection has gone every later
        // command on it fails too, and the last message is the least
        // informative one there is.
        if (!link.deviceDead) {
            link.lastError = std::string("the connection to the rtl_tcp server was lost while ") +
                             what + (detail.empty() ? std::string() : (" (" + detail + ")"));
            link.deadWhat = what;
        }
        link.faulted = true;
        link.deviceDead = true;
    }
    // NEVER THE HOST. A transport error is "the rtl_tcp server closed the
    // connection" or "receive failed (network error N)", which carry none, and
    // the other detail is a fixed sentence.
    if (!detail.empty()) {
        core::diagWarnf("rtltcp: failed while %s: %s", what, detail.c_str());
    } else {
        core::diagWarnf("rtltcp: failed while %s", what);
    }
}

void RtlTcpSource::noteFault(const char* what, const std::string& detail) {
    noteFaultOn(*link_, what, detail);
}

bool RtlTcpSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool RtlTcpSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string RtlTcpSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* RtlTcpSource::lastError() const {
    // A per-THREAD snapshot: the reader rewrites the member while the GUI
    // reads the pointer, and a pointer into a string another thread is
    // assigning is undefined behaviour.
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        snapshot = link_->lastError;
    }
    return snapshot.c_str();
}

void RtlTcpSource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* RtlTcpSource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- commands --------------------------------------------------------------

namespace {

// One five-byte command on the wire: the id, then the value big-endian.
bool sendWire(iiod::Transport& t, std::uint8_t id, std::uint32_t value, std::string& error) {
    const std::uint8_t b[5] = {id,
                               static_cast<std::uint8_t>(value >> 24),
                               static_cast<std::uint8_t>(value >> 16),
                               static_cast<std::uint8_t>(value >> 8),
                               static_cast<std::uint8_t>(value)};
    if (t.sendAll(b, sizeof(b))) { return true; }
    error = t.lastError();
    return false;
}

}  // namespace

bool RtlTcpSource::sendCommandLocked(std::uint8_t id, std::uint32_t value) {
    // NOT OPEN (no socket): the wish is remembered in the member the caller
    // set, and there is nowhere to send it. Not a failure.
    if (transport_ == nullptr) { return true; }
    std::string err;
    if (sendWire(*transport_, id, value, err)) { return true; }
    noteFault("sending a command", err);
    return false;
}

bool RtlTcpSource::isR82xx() const {
    const std::uint32_t t = tunerType_.load(std::memory_order_relaxed);
    return t == kTunerR820T || t == kTunerR828D;
}

double RtlTcpSource::snapGainDb(double db) const {
    // devMutex_ held (ladderTenths_). The nearest rung, the lower one on a tie.
    if (ladderTenths_.empty()) { return db; }
    const double want = db * 10.0;
    int best = ladderTenths_.front();
    for (const int r : ladderTenths_) {
        if (std::fabs(r - want) < std::fabs(best - want)) { best = r; }
    }
    return best / 10.0;
}

bool RtlTcpSource::sendGainLocked() {
    if (isR82xx()) {
        return sendCommandLocked(kCmdTunerGain, static_cast<std::uint32_t>(gainTenths_));
    }
    if (gainCount_.load(std::memory_order_relaxed) > 0) {
        return sendCommandLocked(kCmdGainByIndex, static_cast<std::uint32_t>(gainIndex_));
    }
    return true;  // a tuner with no steps: nothing to choose between
}

// THE WHOLE STATE, in the order the file header gives. Returns false with the
// transport's words in `error` and does NOT latch a fault - the caller decides
// whether this is an open that failed or a start that did.
bool RtlTcpSource::sendStateLocked(std::string& err) {
    const auto put = [&](std::uint8_t id, std::uint32_t v) {
        return sendWire(*transport_, id, v, err);
    };
    bool ok = put(kCmdSampleRate, static_cast<std::uint32_t>(std::llround(rateHz_))) &&
              put(kCmdFrequency, static_cast<std::uint32_t>(std::llround(centerHz_))) &&
              put(kCmdFrequencyCorrection, static_cast<std::uint32_t>(ppm_)) &&
              put(kCmdAgcMode, 0u) && put(kCmdGainMode, autoWanted_ ? 0u : 1u);
    if (ok && !autoWanted_) {
        if (isR82xx()) {
            ok = put(kCmdTunerGain, static_cast<std::uint32_t>(gainTenths_));
        } else if (gainCount_.load(std::memory_order_relaxed) > 0) {
            ok = put(kCmdGainByIndex, static_cast<std::uint32_t>(gainIndex_));
        }
    }
    return ok && put(kCmdBiasTee, biasWanted_ ? 1u : 0u);
}

// --- open / close ----------------------------------------------------------

// The part of an open that follows the dial: read and check the header, work
// out the gain ladder, send the whole state. `t` is already connected.
bool RtlTcpSource::handshakeLocked(std::unique_ptr<iiod::Transport> t, std::string& error) {
    std::uint8_t header[kHeaderBytes];
    if (!t->recvAll(header, sizeof(header))) {
        error = "nothing at " + hostPortText(host_, port_) + " answered as an rtl_tcp server (" +
                t->lastError() + ")";
        return false;
    }
    if (std::memcmp(header, "RTL0", 4) != 0) {
        error = "nothing at " + hostPortText(host_, port_) +
                " speaks rtl_tcp (it answered, but not with an rtl_tcp header)";
        return false;
    }
    tunerType_.store(readBe32(header + 4), std::memory_order_relaxed);
    gainCount_.store(readBe32(header + 8), std::memory_order_relaxed);

    // The gain ladder follows the tuner, and so does the chosen setting: a
    // middling manual gain to start from - the nearest rung to 30 dB, or the
    // middle of an index tuner's table.
    ladderTenths_.clear();
    if (isR82xx()) {
        ladderTenths_ = TunerR82xx::aggregateLadderTenthDb();
        gainTenths_ = static_cast<int>(std::lround(snapGainDb(kDefaultGainDb) * 10.0));
        gainValue_.store(gainTenths_ / 10.0, std::memory_order_relaxed);
    } else {
        const std::uint32_t n = gainCount_.load(std::memory_order_relaxed);
        gainIndex_ = n == 0 ? 0 : static_cast<int>(n / 2);
        gainValue_.store(gainIndex_, std::memory_order_relaxed);
    }

    transport_ = std::move(t);
    std::string sendError;
    if (!sendStateLocked(sendError)) {
        error = "the rtl_tcp server stopped answering while it was being set up (" + sendError +
                ")";
        transport_.reset();
        return false;
    }
    return true;
}

std::unique_ptr<iiod::Transport> RtlTcpSource::dialLocked(std::string& error) {
    iiod::PeerWording peer;
    peer.name = "the rtl_tcp server";
    // WORDED SO health::classifyRadioOpen FILES THEM RIGHT: "nothing at" is the
    // "absent" class, "did not answer" / "timed out" the "timeout" class, so the
    // anonymous counts say "no server there" and "server not answering" rather
    // than "other".
    peer.unknownHostHint = "nothing at that name was found - is rtl_tcp running there, and is the "
                           "address right?";
    peer.unreachableHint = "nothing at that address accepted the connection - is rtl_tcp running "
                           "there?";
    return iiod::connectTcp(host_, port_, iiod::kConnectWait, iiod::kReplyWait, error, peer);
}

bool RtlTcpSource::open(const std::string& args) { return openImpl(args, nullptr); }

bool RtlTcpSource::openWithTransportForTest(const std::string& args,
                                            std::unique_ptr<iiod::Transport> transport) {
    return openImpl(args, std::move(transport));
}

bool RtlTcpSource::openImpl(const std::string& args, std::unique_ptr<iiod::Transport> injected) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (transport_ != nullptr || openMirror_.load(std::memory_order_relaxed)) {
        setError("this rtl_tcp source already has a connection open");
        return false;
    }
    // A SOURCE THAT LOST ITS CONNECTION (or abandoned a reader) is finished:
    // its reader may still be inside the link it shares, and the receiver makes
    // a fresh source for every open.
    if (deviceDead()) { return false; }

    std::string host;
    std::uint16_t port = kRtlTcpDefaultPort;
    std::string error;
    if (!parseRtlTcpAddress(args, host, port, error)) {
        setError(error);
        return false;
    }
    host_ = host;
    port_ = port;
    clearError();

    // FIRST-CONNECT DEFAULTS, the state the open must leave: the native
    // driver's default rate, a middling manual gain (handshakeLocked picks it
    // once the header has said which tuner), no correction, no bias tee.
    rateHz_ = 2048000.0;
    centerHz_ = 100000000.0;
    ppm_ = 0;
    biasWanted_ = false;
    autoWanted_ = false;
    gainTenths_ = 0;
    gainIndex_ = 0;
    gainValue_.store(0.0, std::memory_order_relaxed);

    std::unique_ptr<iiod::Transport> t = std::move(injected);
    if (t == nullptr) {
        t = dialLocked(error);
        if (t == nullptr) {
            setError(error);
            return false;
        }
    }
    if (!handshakeLocked(std::move(t), error)) {
        setError(error);
        return false;
    }

    sampleRateHz_.store(rateHz_, std::memory_order_relaxed);
    centerFrequencyHz_.store(centerHz_, std::memory_order_relaxed);
    autoGain_.store(false, std::memory_order_relaxed);
    biasTee_.store(false, std::memory_order_relaxed);
    // THE ADDRESS IS NOT IN THE NAME: name() reaches the diagnostic log (the
    // GUI quotes it in "the %s refused a tune"), and who is running the server
    // is nobody's business but the user's. The address is in the box the user
    // typed it into, and in the busy label.
    setName(std::string("rtl_tcp server: ") + rtlTcpTunerName(tunerType_.load()) + " (network)");
    openMirror_.store(true, std::memory_order_relaxed);

    // THE READER LIVES FROM HERE TO closeDevice(), streaming or not (see the
    // file header): it drains the socket and keeps the samples only between
    // start() and stop().
    startReaderLocked();

    // THE HOST IS NOT IN THIS LINE. The tuner and the port say what kind of
    // server it was and which service.
    core::diagLogf("rtltcp: opened %s on port %u - %u tuner gain steps",
                   rtlTcpTunerName(tunerType_.load()), static_cast<unsigned>(port_),
                   static_cast<unsigned>(gainCount_.load()));
    return true;
}

void RtlTcpSource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopReaderLocked();
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("rtl_tcp: (not connected)");
}

// --- the reader's life: open() to closeDevice() ----------------------------

void RtlTcpSource::startReaderLocked() {
    link_->transport = transport_.get();
    {
        // A fresh open starts from an empty ring and a closed gate.
        std::lock_guard<std::mutex> gate(link_->gateMutex);
        std::lock_guard<std::mutex> consumer(link_->consumerMutex);
        link_->streaming.store(false, std::memory_order_relaxed);
        std::complex<float> scratch[256];
        while (link_->ring.read(scratch, 256) > 0) {}
    }
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&RtlTcpSource::readerThreadBody, link_);
}

void RtlTcpSource::stopReaderLocked() {
    {
        std::lock_guard<std::mutex> gate(link_->gateMutex);
        link_->streaming.store(false, std::memory_order_relaxed);
    }
    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();

    if (reader_.joinable()) {
        // WAKE A READER PARKED IN recv FIRST: shutdown() on the socket (not a
        // close - the descriptor stays valid under it) ends its receive at
        // once, so the join below is normally instant instead of waiting out
        // the socket's own 2 s bound. Then a BOUNDED JOIN (see PlutoSource's
        // header): longer than iiod::kReplyWait, because the reader's longest
        // legitimate stall is one receive.
        if (transport_ != nullptr) { transport_->shutdown(); }
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, kReaderJoinWait, [this] { return link_->exited; });
        }
        if (exited) {
            reader_.join();
        } else {
            // ABANDONED: the thread keeps its link and the socket pointer
            // inside it, so the socket is leaked deliberately rather than
            // closed under a thread still inside it. The fault condemns this
            // source: every call refuses on a dead device.
            g_readersAbandoned.fetch_add(1, std::memory_order_relaxed);
            noteFault("waiting for the sample reader to stop",
                      "the reader did not return; FoxSDR must be restarted to use this "
                      "source again");
            reader_.detach();
            (void)transport_.release();
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }
    // CLOSING THE SOCKET RELEASES THE SERVER (it serves one client at a time).
    if (transport_ != nullptr) {
        transport_->close();
        transport_.reset();
        link_->transport = nullptr;
    }
    running_.store(false, std::memory_order_relaxed);
}

// --- start / stop ----------------------------------------------------------
//
// THE CONNECTION AND THE READER OUTLIVE A STOP. stop() flips one flag and
// returns; the reader goes on draining the socket and throws what it receives
// away, so the server is never back-pressured and nothing here can block on the
// network. start() clears what is left in the ring and flips the flag back.
// Nothing is reconnected and nothing is replayed: the server's state never
// changed (every setter sends at once, stopped or not).

bool RtlTcpSource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("start() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    {
        // UNDER THE GATE the reader writes through, so no pre-stop chunk can
        // land in the ring between the clear and the flag; and under the
        // consumer lock so a read() on another thread is not a second consumer.
        std::lock_guard<std::mutex> gate(link_->gateMutex);
        {
            std::lock_guard<std::mutex> consumer(link_->consumerMutex);
            std::complex<float> scratch[256];
            while (link_->ring.read(scratch, 256) > 0) {}
        }
        link_->streaming.store(true, std::memory_order_relaxed);
    }
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void RtlTcpSource::stop() {
    // No devMutex_ and no join: this must return at once whatever the network
    // is doing. Under the gate so the reader is not mid-write when it returns.
    {
        std::lock_guard<std::mutex> gate(link_->gateMutex);
        link_->streaming.store(false, std::memory_order_relaxed);
    }
    link_->waitCv.notify_all();  // a read() waiting for samples need not wait out its bound
    running_.store(false, std::memory_order_relaxed);
}

// --- the reader thread -----------------------------------------------------

void RtlTcpSource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    std::vector<std::uint8_t> raw(kRecvBytes);
    std::vector<std::complex<float>> conv(kRecvBytes / 2 + 1);
    // AN ODD BYTE AT THE END OF A RECEIVE IS HALF A PAIR, and it is carried
    // into the next one. TCP delivers however many bytes it likes; dropping
    // the byte, or treating the next receive's first as an I, would swap I and
    // Q for the rest of the stream. Kept whether or not anything is streaming,
    // so a stop in mid-pair cannot misalign the next start.
    std::size_t carry = 0;

    while (link->run.load(std::memory_order_relaxed)) {
        std::size_t got = 0;
        const bool ok = link->transport->recvSome(raw.data() + carry, raw.size() - carry, got);
        if (!link->run.load(std::memory_order_relaxed)) { break; }
        if (!ok) {
            // A FAILED RECEIVE IS THE SERVER GOING (or the network): nothing to
            // retry, and a stream cannot be resynchronised. Fault and leave;
            // the pipeline's source loop polls faulted().
            noteRead(*link, false, 0, false);
            noteFaultOn(*link, "reading samples", link->transport->lastError());
            break;
        }
        const std::size_t total = carry + got;
        const std::size_t pairs = total / 2;
        for (std::size_t i = 0; i < pairs; ++i) {
            conv[i] = std::complex<float>(kByteToFloat.v[raw[2 * i]], kByteToFloat.v[raw[2 * i + 1]]);
        }
        carry = total - pairs * 2;
        if (carry != 0) { raw[0] = raw[total - 1]; }

        bool dropped = false;
        bool kept = false;
        if (pairs > 0) {
            // THE GATE: samples are kept only while streaming, and stop() and
            // start() flip it under this same lock, so a chunk received before
            // a stop can never reach the ring after it. Held for one memcpy.
            std::lock_guard<std::mutex> gate(link->gateMutex);
            if (link->streaming.load(std::memory_order_relaxed)) {
                kept = true;
                if (link->ring.write(conv.data(), pairs) != pairs) {
                    // The host fell behind, not the server: counted, never silent.
                    dropped = true;
                    link->dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // NOT STREAMING: the samples are discarded here, and the socket
            // stays drained so the server is never back-pressured.
        }
        noteRead(*link, true, pairs, dropped);
        if (!kept) { continue; }
        {
            // Taken and released so a read() that has just tested the ring and
            // is about to wait cannot miss this notify in between.
            std::lock_guard<std::mutex> lk(link->waitMutex);
        }
        link->waitCv.notify_all();
    }

    // LAST ACT: what the bounded join waits for.
    {
        std::lock_guard<std::mutex> lk(link->waitMutex);
        link->run.store(false, std::memory_order_relaxed);
        link->exited = true;
    }
    link->waitCv.notify_all();
}

void RtlTcpSource::noteRead(ReaderLink& link, bool ok, std::size_t samples, bool dropped) {
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    bool warn = false;
    {
        std::lock_guard<std::mutex> lk(link.healthMutex);
        StreamHealth& h = link.health;
        if (!h.windowOpen) {
            h.windowOpen = true;
            h.windowStart = now;
            h.lastSamples = now;
        }
        ++h.reads;
        if (ok && samples > 0) {
            ++h.withSamples;
            h.samples += samples;
            const auto gap =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
            if (gap > h.longestGapMs) { h.longestGapMs = gap; }
            h.lastSamples = now;
        } else if (ok) {
            ++h.timeouts;  // a receive that carried less than one sample
        } else {
            ++h.errors;
        }
        if (dropped) { ++h.overflows; }

        if (now - h.windowStart < link.healthWindow) { return; }

        const bool nominal = h.timeouts == 0 && h.overflows == 0 && h.errors == 0 &&
                             h.longestGapMs < 250;
        const bool first = !link.healthEverWritten;
        warn = h.errors > 0 || h.longestGapMs >= 1000;
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (!(warn || first || !nominal)) { return; }
        link.healthEverWritten = true;
    }
    if (warn) {
        core::diagWarnf("%s", line.c_str());
    } else {
        core::diagLogf("%s", line.c_str());
    }
}

std::string RtlTcpSource::healthLineLocked(ReaderLink& link) {
    StreamHealth& h = link.health;
    if (!h.windowOpen || h.reads == 0) { return std::string(); }
    const auto now = std::chrono::steady_clock::now();
    const auto openGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
    if (openGap > h.longestGapMs) { h.longestGapMs = openGap; }
    const auto windowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.windowStart).count();
    char buf[192];
    // THE FORMAT SoapySource::streamHealthLine WRITES, word for word.
    std::snprintf(buf, sizeof(buf),
                  "source: stream health - reads %llu, with samples %llu, timeouts %llu, "
                  "overflows %llu, errors %llu, longest gap %lld ms, %llu samples in %lld s",
                  static_cast<unsigned long long>(h.reads),
                  static_cast<unsigned long long>(h.withSamples),
                  static_cast<unsigned long long>(h.timeouts),
                  static_cast<unsigned long long>(h.overflows),
                  static_cast<unsigned long long>(h.errors),
                  static_cast<long long>(h.longestGapMs),
                  static_cast<unsigned long long>(h.samples),
                  static_cast<long long>((windowMs + 500) / 1000));
    h = StreamHealth{};
    return std::string(buf);
}

std::string RtlTcpSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void RtlTcpSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t RtlTcpSource::droppedBuffers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long RtlTcpSource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

// --- read ------------------------------------------------------------------

std::size_t RtlTcpSource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    const auto take = [this, dst, n] {
        // The consumer side of the ring, under a lock so start()'s clear is
        // never a second consumer.
        std::lock_guard<std::mutex> lk(link_->consumerMutex);
        return link_->ring.read(dst, n);
    };
    std::size_t got = take();
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, kReadWait, [this] {
            return link_->ring.size() > 0 || !link_->streaming.load(std::memory_order_relaxed) ||
                   !link_->run.load(std::memory_order_relaxed);
        });
    }
    return take();
}

// --- rate, frequency, gain, bias tee ---------------------------------------

std::vector<double> RtlTcpSource::supportedSampleRatesHz() const {
    return Rtl2832u::supportedRatesHz();
}

bool RtlTcpSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("setSampleRateHz() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    const std::vector<double> rates = Rtl2832u::supportedRatesHz();
    double best = rates.front();
    for (const double r : rates) {
        if (std::fabs(r - hz) < std::fabs(best - hz)) { best = r; }
    }
    rateHz_ = best;
    sampleRateHz_.store(best, std::memory_order_relaxed);
    return sendCommandLocked(kCmdSampleRate, static_cast<std::uint32_t>(std::llround(best)));
}

bool RtlTcpSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("setCenterFrequencyHz() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    // The wire carries a 32-bit Hz value. Refused rather than clamped: a tune
    // that lands somewhere else is worse than one that does not happen.
    if (!(hz > 0.0) || hz > 4294967295.0) {
        setError("the rtl_tcp protocol can tune 1 Hz to 4294.967 MHz");
        return false;
    }
    centerHz_ = std::floor(hz + 0.5);
    centerFrequencyHz_.store(centerHz_, std::memory_order_relaxed);
    return sendCommandLocked(kCmdFrequency, static_cast<std::uint32_t>(centerHz_));
}

bool RtlTcpSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    if (tunerType_.load(std::memory_order_relaxed) == kTunerE4000) {
        loHz = kE4000LoHz;
        hiHz = kE4000HiHz;
    } else {
        loHz = kR82xxLoHz;
        hiHz = kR82xxHiHz;
    }
    return true;
}

bool RtlTcpSource::setFrequencyCorrectionPpm(double ppm) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("setFrequencyCorrectionPpm() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    ppm_ = core::ppmWholeForRadio(ppm);
    // Two's complement in the u32: -3 goes as 0xFFFFFFFD.
    if (!sendCommandLocked(kCmdFrequencyCorrection, static_cast<std::uint32_t>(ppm_))) {
        return false;
    }
    // The radio must be left ON the frequency this driver reports (IqSource):
    // what a server does with the centre frequency when the correction moves
    // is its own business, so it is told again.
    return sendCommandLocked(kCmdFrequency, static_cast<std::uint32_t>(centerHz_));
}

bool RtlTcpSource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("setBiasT() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    biasWanted_ = on;
    if (!sendCommandLocked(kCmdBiasTee, on ? 1u : 0u)) { return false; }
    biasTee_.store(on, std::memory_order_relaxed);
    return true;
}

std::vector<GainInfo> RtlTcpSource::gains() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    GainInfo g;
    g.name = "TUNER";
    if (isR82xx()) {
        // Real decibels, on the native driver's own ladder.
        g.minDb = ladderTenths_.empty() ? 0.0 : ladderTenths_.front() / 10.0;
        g.maxDb = ladderTenths_.empty() ? 0.0 : ladderTenths_.back() / 10.0;
        g.stepDb = 0.1;
        g.unit = GainUnit::Decibels;
    } else {
        // Steps: the server publishes how many, and no decibel figure for any.
        const std::uint32_t n = gainCount_.load(std::memory_order_relaxed);
        g.minDb = 0.0;
        g.maxDb = n == 0 ? 0.0 : static_cast<double>(n - 1);
        g.stepDb = 1.0;
        g.unit = GainUnit::Steps;
    }
    return {g};
}

bool RtlTcpSource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("setGainDb() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!equalsNoCase(gainName, "TUNER")) {
        setError("the rtl_tcp source has no gain called \"" + gainName + "\"");
        return false;
    }
    if (!(db == db)) {  // NaN
        setError("setGainDb() requires a number");
        return false;
    }
    // A HAND-SET GAIN MEANS MANUAL GAIN: the server ignores 0x04 and 0x0d
    // while the tuner is in automatic mode, so a slider moved with it on
    // would move on screen and change nothing.
    if (autoWanted_) {
        autoWanted_ = false;
        autoGain_.store(false, std::memory_order_relaxed);
        if (!sendCommandLocked(kCmdGainMode, 1u)) { return false; }
    }
    if (isR82xx()) {
        gainTenths_ = static_cast<int>(std::lround(snapGainDb(db) * 10.0));
        gainValue_.store(gainTenths_ / 10.0, std::memory_order_relaxed);
    } else {
        const std::uint32_t n = gainCount_.load(std::memory_order_relaxed);
        const int top = n == 0 ? 0 : static_cast<int>(n) - 1;
        gainIndex_ = std::max(0, std::min(top, static_cast<int>(std::lround(db))));
        gainValue_.store(gainIndex_, std::memory_order_relaxed);
    }
    return sendGainLocked();
}

double RtlTcpSource::gainDb(const std::string& gainName) const {
    if (equalsNoCase(gainName, "TUNER")) { return gainValue_.load(std::memory_order_relaxed); }
    return 0.0;
}

bool RtlTcpSource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("setAutoGain() called with no rtl_tcp server open");
        return false;
    }
    if (deviceDead()) { return false; }
    autoWanted_ = on;
    autoGain_.store(on, std::memory_order_relaxed);
    if (!sendCommandLocked(kCmdGainMode, on ? 0u : 1u)) { return false; }
    // Off: put the chosen manual level back, so what the slider shows is what
    // the tuner is doing rather than whatever the server last kept.
    if (!on) { return sendGainLocked(); }
    return true;
}

}  // namespace cascade::source
