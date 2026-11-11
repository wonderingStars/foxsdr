// soapy_remote_source.cpp - see soapy_remote_source.hpp and soapy_remote_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_remote_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace cascade::source {

namespace sr = soapyremote;

namespace {

constexpr int kDirRx = sr::kDirectionRx;
constexpr std::size_t kMaxDatagram = 65536;
// The window (bytes) and MTU announced to the server; the flow-control ACK allows
// window / mtu datagrams in flight, as the endpoint's constructor derives it.
constexpr std::size_t kStreamMtu = 1500;
constexpr std::size_t kStreamWindow = 1u << 20;
// The receive slice of the stream sockets, ms: the longest a reader can be parked before it
// looks at the close flag.
constexpr int kStreamPollMs = 200;

bool timedOut(const char* err) { return err != nullptr && std::strstr(err, "timed out") != nullptr; }

}  // namespace

bool parseSoapyRemoteAddress(const std::string& text, std::string& host, std::uint16_t& port,
                             std::string& error) {
    return parseHostPort(text, sr::kDefaultPort, host, port, error);
}

// ---------------------------------------------------------------------------------------
// the stream half
// ---------------------------------------------------------------------------------------

struct SoapyRemoteSource::Stream {
    std::shared_ptr<iiod::Transport> data;
    std::shared_ptr<iiod::Transport> status;  // held open, never read (statuses are advisory)
    bool datagram = false;
    std::string format;
    double scale = 1.0;
    std::size_t elemBytes = 0;
    std::uint32_t maxInFlight = 0;
    std::uint32_t triggerWindow = 1;
    std::uint32_t lastRecv = 0;
    std::uint32_t lastSend = 0;
    bool gotFirst = false;
    int silentTicks = 0;
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(kMaxDatagram);
    std::vector<std::complex<float>> out;
    std::atomic<std::uint64_t> statusDatagrams{0};

    void sendAck() {
        sr::DatagramHeader h;
        h.bytes = sr::kDatagramHeaderBytes;
        h.sequence = lastRecv;
        h.elems = static_cast<std::int32_t>(maxInFlight);
        std::uint8_t raw[sr::kDatagramHeaderBytes];
        sr::encodeDatagramHeader(h, raw);
        data->sendAll(raw, sizeof(raw));  // a failed ack shows as a stalled stream, not here
        lastSend = lastRecv;
    }

    void convert(const std::uint8_t* p, std::size_t n) {
        out.resize(n);
        const float inv = static_cast<float>(1.0 / scale);
        if (format == "CS8") {
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = {static_cast<float>(static_cast<std::int8_t>(p[2 * i])) * inv,
                          static_cast<float>(static_cast<std::int8_t>(p[2 * i + 1])) * inv};
            }
        } else if (format == "CU8") {
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = {static_cast<float>(static_cast<int>(p[2 * i]) - 127) * inv,
                          static_cast<float>(static_cast<int>(p[2 * i + 1]) - 127) * inv};
            }
        } else if (format == "CS16") {
            for (std::size_t i = 0; i < n; ++i) {
                const std::int16_t a = static_cast<std::int16_t>(p[4 * i] | (p[4 * i + 1] << 8));
                const std::int16_t b = static_cast<std::int16_t>(p[4 * i + 2] | (p[4 * i + 3] << 8));
                out[i] = {static_cast<float>(a) * inv, static_cast<float>(b) * inv};
            }
        } else {  // CF32, little-endian floats
            for (std::size_t i = 0; i < n; ++i) {
                float a = 0.0f, b = 0.0f;
                std::memcpy(&a, p + 8 * i, 4);
                std::memcpy(&b, p + 8 * i + 4, 4);
                out[i] = {a * inv, b * inv};
            }
        }
    }

    // How the reader waits: in slices of kPollMs, so that a close is noticed within one slice
    // even when the peer never answers the FIN the wake-up sends (shutdown() alone does not
    // end a receive on every platform).
    std::size_t have = 0;       // bytes of the datagram being assembled (TCP) so far
    int silentLimit = 3;        // silent slices that make a streaming source's server "lost"

    // One datagram (UDP) or a piece of one (TCP: the stream is reassembled across receive
    // slices, so a slice that times out in the middle of a header loses nothing).
    bool pump(NetLink& link) {
        std::size_t complete = 0;  // bytes of a finished datagram in buf, 0 if none yet
        bool ok = false;
        if (datagram) {
            std::size_t got = 0;
            ok = data->recvSome(buf.data(), buf.size(), got);
            complete = got;
        } else {
            std::size_t want = sr::kDatagramHeaderBytes - std::min<std::size_t>(have, sr::kDatagramHeaderBytes);
            if (have >= sr::kDatagramHeaderBytes) {
                want = sr::decodeDatagramHeader(buf.data()).bytes - have;
            }
            std::size_t got = 0;
            ok = want == 0 ? true : data->recvSome(buf.data() + have, want, got);
            have += got;
            if (ok && have >= sr::kDatagramHeaderBytes) {
                const sr::DatagramHeader h = sr::decodeDatagramHeader(buf.data());
                // The length field is the peer's word: checked before it sizes a receive.
                if (h.bytes < sr::kDatagramHeaderBytes || h.bytes > buf.size()) {
                    link.fault("the radio server sent a bad stream header",
                               "a datagram length of " + std::to_string(h.bytes) + " bytes");
                    return false;
                }
                if (have == h.bytes) {
                    complete = have;
                    have = 0;
                } else {
                    silentTicks = 0;
                    return true;  // more of this datagram to come
                }
            } else if (ok) {
                silentTicks = 0;
                return true;
            }
        }
        if (!ok) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }  // we are closing
            if (timedOut(data->lastError())) {
                if (!gotFirst && datagram) { sendAck(); }  // the gratuitous ack, until data flows
                // Idle (not activated) is silence by design; a streaming source that has been
                // silent for several slices has lost its server.
                if (link.streaming.load(std::memory_order_relaxed) && ++silentTicks >= silentLimit) {
                    link.fault("the radio server stopped sending", "no stream data arrived");
                    return false;
                }
                return true;
            }
            link.fault("the radio server connection failed", data->lastError());
            return false;
        }
        silentTicks = 0;
        gotFirst = true;
        return handle(link, complete);
    }

    bool handle(NetLink& link, std::size_t completeBytes) {
        const sr::DatagramHeader h = sr::decodeDatagramHeader(buf.data());
        switch (sr::checkDatagram(h, completeBytes, elemBytes)) {
            case sr::DatagramVerdict::Ok: break;
            case sr::DatagramVerdict::Status:
                // SoapySDR's TIMEOUT (-1), CORRUPTION (-3), OVERFLOW (-4), UNDERFLOW (-7) are
                // the server saying it lost samples: counted, not fatal. Anything else is.
                statusDatagrams.fetch_add(1, std::memory_order_relaxed);
                if (h.elems == -1 || h.elems == -3 || h.elems == -4 || h.elems == -7) {
                    link.dropped.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                link.fault("the radio server reported a stream error",
                           "code " + std::to_string(h.elems));
                return false;
            default:
                link.fault("the radio server sent a malformed stream datagram",
                           "its length or sample count does not fit the data received");
                return false;
        }
        convert(buf.data() + sr::kDatagramHeaderBytes, static_cast<std::size_t>(h.elems));
        link.pushSamples(out.data(), out.size());
        lastRecv = h.sequence + 1;
        if (lastRecv - lastSend >= triggerWindow) { sendAck(); }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

SoapyRemoteSource::~SoapyRemoteSource() { closeDevice(); }

bool SoapyRemoteSource::failOpen(const std::string& why) {
    setError(why);
    teardownLocked();
    return false;
}

sr::Unpacker SoapyRemoteSource::callLocked(const sr::Packer& request) {
    if (control_ == nullptr) { throw std::runtime_error("not connected"); }
    if (!sr::sendMessage(*control_, request.finish())) { throw std::runtime_error(control_->lastError()); }
    std::vector<std::uint8_t> payload;
    std::uint32_t version = 0;
    std::string err;
    if (!sr::recvMessage(*control_, payload, version, err)) { throw std::runtime_error(err); }
    return sr::Unpacker(std::move(payload), version);
}

bool SoapyRemoteSource::open(const std::string& args) { return openImpl(args, nullptr); }

bool SoapyRemoteSource::openWithTransportForTest(const std::string& args,
                                                 std::shared_ptr<iiod::Transport> control) {
    return openImpl(args, std::move(control));
}

bool SoapyRemoteSource::openImpl(const std::string& args, std::shared_ptr<iiod::Transport> injected) {
    closeDevice();
    std::lock_guard<std::mutex> lk(mutex_);
    std::string addr = argValue(args, "soapyremote");
    std::string host;
    std::uint16_t port = sr::kDefaultPort;
    std::string err;
    if (addr.empty()) { addr = "127.0.0.1"; }
    if (!parseSoapyRemoteAddress(addr, host, port, err)) { setError(err); return false; }
    host_ = host;
    prot_ = argValue(args, "prot");
    if (prot_.empty()) { prot_ = "tcp"; }
    if (prot_ != "tcp" && prot_ != "udp") {
        setError("the stream protocol must be tcp or udp");
        return false;
    }
    const std::string to = argValue(args, "timeout");
    ioWaitMs_ = 2000;
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        ioWaitMs_ = static_cast<int>(v);
    }
    std::map<std::string, std::string> devArgs;
    devArgs["soapy_remote_no_deeper"] = "";
    {
        const std::string dev = argValue(args, "device");
        std::size_t pos = 0;
        while (pos < dev.size()) {
            std::size_t end = dev.find(';', pos);
            if (end == std::string::npos) { end = dev.size(); }
            const std::string pair = dev.substr(pos, end - pos);
            const std::size_t c = pair.find(':');
            if (c != std::string::npos && c > 0) { devArgs[pair.substr(0, c)] = pair.substr(c + 1); }
            pos = end + 1;
        }
    }

    if (injected != nullptr) {
        control_ = std::move(injected);
    } else {
        std::string cerr;
        iiod::PeerWording w;
        w.name = "the SoapyRemote server";
        w.unknownHostHint = "is the address right, and is SoapySDRServer running?";
        std::unique_ptr<iiod::Transport> t =
            iiod::connectTcp(host, port, std::chrono::milliseconds(3000),
                             std::chrono::milliseconds(ioWaitMs_), cerr, w);
        if (t == nullptr) { setError(cerr); return false; }
        control_ = std::move(t);
    }

    try {
        sr::Packer p;
        p.call(sr::C_MAKE).kwargs(devArgs);
        callLocked(p);
        made_ = true;
        queryDeviceLocked();
    } catch (const std::exception& e) {
        return failOpen(std::string("the SoapyRemote server refused the device: ") + e.what());
    }
    if (sr::elementBytes(remoteFormat_) == 0) {
        return failOpen("the server's stream format " + remoteFormat_ +
                        " is not one FoxSDR reads (CS8, CU8, CS16 and CF32 are)");
    }
    if (!setupStreamLocked(prot_)) { return failOpen(lastError()); }
    // Defaults the receiver expects to find after an open.
    if (!rates_.empty()) { rateHz_ = rates_.front(); }
    markOpen(true);
    setName("SoapyRemote: " + host + (injected ? "" : ":" + std::to_string(port)));
    return true;
}

void SoapyRemoteSource::queryDeviceLocked() {
    {
        sr::Packer p;
        p.call(sr::C_GET_NATIVE_STREAM_FORMAT).chr(static_cast<char>(kDirRx)).i32(0);
        sr::Unpacker u = callLocked(p);
        remoteFormat_ = u.str();
        fullScale_ = u.f64();
        if (!(fullScale_ > 0.0) || !std::isfinite(fullScale_)) { throw std::runtime_error("a full-scale of zero"); }
    }
    // The rest is information for the screen: a server that answers an error to any of these
    // still opens (the stream is what matters).
    const auto soft = [this](auto&& fn) {
        try { fn(); } catch (const std::exception&) {}
    };
    soft([&] {
        sr::Packer p;
        p.call(sr::C_LIST_SAMPLE_RATES).chr(static_cast<char>(kDirRx)).i32(0);
        rates_ = callLocked(p).f64List();
    });
    soft([&] {
        sr::Packer p;
        p.call(sr::C_GET_FREQUENCY_RANGE).chr(static_cast<char>(kDirRx)).i32(0);
        freqRanges_ = callLocked(p).rangeList();
    });
    soft([&] {
        sr::Packer p;
        p.call(sr::C_LIST_ANTENNAS).chr(static_cast<char>(kDirRx)).i32(0);
        antennas_ = callLocked(p).stringList();
        if (!antennas_.empty()) { antenna_ = antennas_.front(); }
    });
    soft([&] {
        sr::Packer p;
        p.call(sr::C_LIST_GAINS).chr(static_cast<char>(kDirRx)).i32(0);
        const std::vector<std::string> names = callLocked(p).stringList();
        for (const std::string& n : names) {
            sr::Packer q;
            q.call(sr::C_GET_GAIN_RANGE_ELEMENT).chr(static_cast<char>(kDirRx)).i32(0).str(n);
            const sr::Range r = callLocked(q).range();
            GainInfo g;
            g.name = n;
            g.minDb = r.minimum;
            g.maxDb = r.maximum;
            g.stepDb = r.step > 0.0 ? r.step : 1.0;
            gains_.push_back(g);
            gainValues_[n] = r.minimum;
        }
    });
    soft([&] {
        sr::Packer p;
        p.call(sr::C_HAS_GAIN_MODE).chr(static_cast<char>(kDirRx)).i32(0);
        hasGainMode_ = callLocked(p).boolean();
    });
    soft([&] {
        sr::Packer p;
        p.call(sr::C_HAS_FREQUENCY_CORRECTION).chr(static_cast<char>(kDirRx)).i32(0);
        hasCorrection_ = callLocked(p).boolean();
    });
}

bool SoapyRemoteSource::setupStreamLocked(const std::string& prot) {
    auto st = std::make_shared<Stream>();
    st->datagram = (prot == "udp");
    st->format = remoteFormat_;
    st->scale = fullScale_;
    st->elemBytes = sr::elementBytes(remoteFormat_);
    st->maxInFlight = static_cast<std::uint32_t>(kStreamWindow / kStreamMtu);
    st->triggerWindow = std::max<std::uint32_t>(1, st->maxInFlight / 8);
    st->silentLimit = std::max(3, ioWaitMs_ / kStreamPollMs);

    std::map<std::string, std::string> sargs;
    sargs["remote:prot"] = prot;
    sargs["remote:mtu"] = std::to_string(kStreamMtu);
    sargs["remote:window"] = std::to_string(kStreamWindow);
    sargs["remote:format"] = remoteFormat_;

    try {
        std::string clientPort, statusPort;
        std::shared_ptr<UdpTransport> udpData, udpStatus;
        if (st->datagram) {
            std::string e;
            udpData = std::shared_ptr<UdpTransport>(UdpTransport::bindLocal("0.0.0.0", 0, std::chrono::milliseconds(kStreamPollMs), e).release());
            if (!udpData) { throw std::runtime_error(e); }
            udpStatus = std::shared_ptr<UdpTransport>(UdpTransport::bindLocal("0.0.0.0", 0, std::chrono::milliseconds(kStreamPollMs), e).release());
            if (!udpStatus) { throw std::runtime_error(e); }
            clientPort = std::to_string(udpData->localPort());
            statusPort = std::to_string(udpStatus->localPort());
        }
        sr::Packer p;
        p.call(sr::C_SETUP_STREAM)
            .chr(static_cast<char>(kDirRx))
            .str(remoteFormat_)
            .sizeList({0})
            .kwargs(sargs)
            .str(clientPort)
            .str(statusPort);
        if (!sr::sendMessage(*control_, p.finish())) { throw std::runtime_error(control_->lastError()); }
        const auto readReply = [this]() {
            std::vector<std::uint8_t> payload;
            std::uint32_t version = 0;
            std::string err;
            if (!sr::recvMessage(*control_, payload, version, err)) { throw std::runtime_error(err); }
            return sr::Unpacker(std::move(payload), version);
        };
        std::string serverPort;
        if (!st->datagram) {
            sr::Unpacker first = readReply();
            serverPort = first.str();
            std::uint16_t portNum = 0;
            std::string e, ignoredHost;
            if (!parseSoapyRemoteAddress("x:" + serverPort, ignoredHost, portNum, e)) { throw std::runtime_error(e); }
            iiod::PeerWording w;
            w.name = "the SoapyRemote stream";
            std::unique_ptr<iiod::Transport> d = iiod::connectTcp(
                host_, portNum, std::chrono::milliseconds(3000), std::chrono::milliseconds(kStreamPollMs), e, w);
            if (!d) { throw std::runtime_error(e); }
            std::unique_ptr<iiod::Transport> s = iiod::connectTcp(
                host_, portNum, std::chrono::milliseconds(3000), std::chrono::milliseconds(kStreamPollMs), e, w);
            if (!s) { throw std::runtime_error(e); }
            st->data = std::move(d);
            st->status = std::move(s);
        }
        sr::Unpacker second = readReply();
        streamId_ = second.i32();
        serverPort = second.str();
        if (st->datagram) {
            std::uint16_t portNum = 0;
            std::string e, ignoredHost;
            if (!parseSoapyRemoteAddress("x:" + serverPort, ignoredHost, portNum, e)) { throw std::runtime_error(e); }
            if (!udpData->connectTo(host_, portNum)) { throw std::runtime_error(udpData->lastError()); }
            st->data = udpData;
            st->status = udpStatus;
        }
    } catch (const std::exception& e) {
        setError(std::string("the SoapyRemote stream could not be set up: ") + e.what());
        return false;
    }
    stream_ = st;
    st->sendAck();  // the gratuitous ack that tells the sender its window
    std::shared_ptr<Stream> keep = st;
    const bool started = beginReader([keep](NetLink& l) { return keep->pump(l); },
                                     [keep]() { keep->data->shutdown(); }, keep);
    return started;
}

void SoapyRemoteSource::teardownLocked() {
    // Best-effort goodbye: every reply is optional, none may throw out of here.
    if (control_ != nullptr) {
        const auto bye = [this](const sr::Packer& p) {
            try { callLocked(p); } catch (const std::exception&) {}
        };
        if (active_ && streamId_ >= 0) {
            sr::Packer p;
            p.call(sr::C_DEACTIVATE_STREAM).i32(streamId_).i32(0).i64(0);
            bye(p);
        }
        if (stream_ != nullptr && streamId_ >= 0) {
            sr::Packer p;
            p.call(sr::C_CLOSE_STREAM).i32(streamId_);
            bye(p);
        }
        if (made_) {
            sr::Packer p;
            p.call(sr::C_UNMAKE);
            bye(p);
            sr::Packer h;
            h.call(sr::C_HANGUP);
            bye(h);
        }
    }
    active_ = false;
    made_ = false;
    streamId_ = -1;
    markOpen(false);
    endReader();
    if (stream_ != nullptr) {
        if (stream_->data) { stream_->data->close(); }
        if (stream_->status) { stream_->status->close(); }
        stream_.reset();
    }
    if (control_ != nullptr) {
        control_->close();
        control_.reset();
    }
}

void SoapyRemoteSource::closeDevice() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (control_ == nullptr && stream_ == nullptr && !isOpen()) { endReader(); return; }
    const std::string keep = lastError();
    teardownLocked();
    if (!keep.empty()) { setError(keep); }
    rates_.clear();
    freqRanges_.clear();
    gains_.clear();
    gainValues_.clear();
    antennas_.clear();
    setName("network radio: (not connected)");
}

bool SoapyRemoteSource::start() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || stream_ == nullptr) { setError("start() called with no SoapyRemote server open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    try {
        sr::Packer p;
        p.call(sr::C_ACTIVATE_STREAM).i32(streamId_).i32(0).i64(0).i32(0);
        const int rc = callLocked(p).i32();
        if (rc != 0) {
            setError("the SoapyRemote server refused to start the stream (error " + std::to_string(rc) + ")");
            return false;
        }
    } catch (const std::exception& e) {
        setError(std::string("starting the SoapyRemote stream failed: ") + e.what());
        return false;
    }
    active_ = true;
    return startKeeping();
}

void SoapyRemoteSource::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopKeeping();
    if (active_ && control_ != nullptr && streamId_ >= 0) {
        try {
            sr::Packer p;
            p.call(sr::C_DEACTIVATE_STREAM).i32(streamId_).i32(0).i64(0);
            callLocked(p).i32();
        } catch (const std::exception& e) {
            setError(std::string("stopping the SoapyRemote stream failed: ") + e.what());
        }
    }
    active_ = false;
}

double SoapyRemoteSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rateHz_;
}

bool SoapyRemoteSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no SoapyRemote server open"); return false; }
    if (!(hz > 0.0) || !std::isfinite(hz)) { setError("setSampleRateHz() requires a positive rate"); return false; }
    double use = hz;
    if (!rates_.empty()) {
        use = rates_.front();
        for (const double r : rates_) {
            if (std::fabs(r - hz) < std::fabs(use - hz)) { use = r; }
        }
    }
    try {
        sr::Packer p;
        p.call(sr::C_SET_SAMPLE_RATE).chr(static_cast<char>(kDirRx)).i32(0).f64(use);
        callLocked(p);
    } catch (const std::exception& e) {
        setError(std::string("setting the sample rate failed: ") + e.what());
        return false;
    }
    rateHz_ = use;
    return true;
}

double SoapyRemoteSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return centerHz_;
}

bool SoapyRemoteSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen()) { setError("setCenterFrequencyHz() called with no SoapyRemote server open"); return false; }
    if (!(hz >= 0.0) || !std::isfinite(hz)) { setError("setCenterFrequencyHz() requires a frequency"); return false; }
    try {
        sr::Packer p;
        p.call(sr::C_SET_FREQUENCY).chr(static_cast<char>(kDirRx)).i32(0).f64(hz).kwargs({});
        callLocked(p);
    } catch (const std::exception& e) {
        setError(std::string("tuning failed: ") + e.what());
        return false;
    }
    centerHz_ = hz;
    return true;
}

bool SoapyRemoteSource::hasFrequencyCorrection() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return hasCorrection_;
}

bool SoapyRemoteSource::setFrequencyCorrectionPpm(double ppm) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !hasCorrection_) { return false; }
    try {
        sr::Packer p;
        p.call(sr::C_SET_FREQUENCY_CORRECTION).chr(static_cast<char>(kDirRx)).i32(0).f64(ppm);
        callLocked(p);
    } catch (const std::exception& e) {
        setError(std::string("setting the frequency correction failed: ") + e.what());
        return false;
    }
    return true;
}

std::vector<GainInfo> SoapyRemoteSource::gains() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return gains_;
}

bool SoapyRemoteSource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen()) { setError("setGainDb() called with no SoapyRemote server open"); return false; }
    if (!std::isfinite(db)) { setError("setGainDb() requires a number"); return false; }
    const GainInfo* g = nullptr;
    for (const GainInfo& x : gains_) {
        if (x.name == name) { g = &x; }
    }
    if (g == nullptr) { setError("this radio has no gain called " + name); return false; }
    const double use = std::min(g->maxDb, std::max(g->minDb, db));
    try {
        sr::Packer p;
        p.call(sr::C_SET_GAIN_ELEMENT).chr(static_cast<char>(kDirRx)).i32(0).str(name).f64(use);
        callLocked(p);
    } catch (const std::exception& e) {
        setError(std::string("setting the gain failed: ") + e.what());
        return false;
    }
    gainValues_[name] = use;
    return true;
}

double SoapyRemoteSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const auto it = gainValues_.find(name);
    return it == gainValues_.end() ? 0.0 : it->second;
}

bool SoapyRemoteSource::autoGainSupported() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return hasGainMode_;
}

bool SoapyRemoteSource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !hasGainMode_) { return false; }
    try {
        sr::Packer p;
        p.call(sr::C_SET_GAIN_MODE).chr(static_cast<char>(kDirRx)).i32(0).boolean(on);
        callLocked(p);
    } catch (const std::exception& e) {
        setError(std::string("setting the gain mode failed: ") + e.what());
        return false;
    }
    autoGain_ = on;
    return true;
}

bool SoapyRemoteSource::autoGain() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return autoGain_;
}

std::vector<std::string> SoapyRemoteSource::antennas() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return antennas_;
}

bool SoapyRemoteSource::setAntenna(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen()) { return false; }
    if (std::find(antennas_.begin(), antennas_.end(), name) == antennas_.end()) {
        setError("this radio has no antenna called " + name);
        return false;
    }
    try {
        sr::Packer p;
        p.call(sr::C_SET_ANTENNA).chr(static_cast<char>(kDirRx)).i32(0).str(name);
        callLocked(p);
    } catch (const std::exception& e) {
        setError(std::string("choosing the antenna failed: ") + e.what());
        return false;
    }
    antenna_ = name;
    return true;
}

std::string SoapyRemoteSource::antenna() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return antenna_;
}

std::vector<double> SoapyRemoteSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rates_;
}

bool SoapyRemoteSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (freqRanges_.empty()) { return false; }
    loHz = freqRanges_.front().minimum;
    hiHz = freqRanges_.front().maximum;
    for (const sr::Range& r : freqRanges_) {
        loHz = std::min(loHz, r.minimum);
        hiHz = std::max(hiHz, r.maximum);
    }
    return true;
}

std::string SoapyRemoteSource::remoteFormat() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return remoteFormat_;
}

double SoapyRemoteSource::remoteFullScale() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fullScale_;
}

std::string SoapyRemoteSource::streamProtocol() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return prot_;
}

}  // namespace cascade::source
