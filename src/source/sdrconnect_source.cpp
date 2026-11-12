// sdrconnect_source.cpp - see sdrconnect_source.hpp, sdrconnect_protocol.hpp and websocket_client.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrconnect_source.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "source/sdrconnect_protocol.hpp"
#include "source/websocket_client.hpp"

namespace cascade::source {

namespace sc = sdrconnect;

namespace {
// The receive slice of the reader, ms: the longest it is parked before it looks at the close flag.
constexpr int kPollMs = 200;
constexpr std::size_t kMaxHandshakeBytes = 8192;
}  // namespace

// ---------------------------------------------------------------------------------------
// the connection: shared between the source and its reader thread
// ---------------------------------------------------------------------------------------

struct SdrConnectSource::Conn {
    std::shared_ptr<iiod::Transport> t;
    ws::FrameParser parser;
    std::mt19937 maskRng{std::random_device{}()};

    std::mutex sendMutex;

    std::mutex propMutex;
    std::condition_variable propCv;
    std::map<std::string, std::string> props;
    std::map<std::string, std::uint64_t> propSeq;
    std::atomic<bool> serverClosed{false};

    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(16384);
    std::vector<std::complex<float>> iq;
    int silentTicks = 0;
    int silentLimit = 10;
    std::atomic<std::uint64_t> binaryOther{0};

    bool sendMessage(ws::Opcode op, const std::uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> lk(sendMutex);
        std::array<std::uint8_t, 4> mask{};
        for (auto& m : mask) { m = static_cast<std::uint8_t>(maskRng()); }
        const std::vector<std::uint8_t> f = ws::encodeClientFrame(op, d, n, mask);
        return t->sendAll(f.data(), f.size());
    }

    void notePush(const sc::Envelope& e) {
        if (e.eventType != "property_changed" && e.eventType != "get_property_response") { return; }
        if (!e.device.empty() && e.device != "primary") { return; }  // the Secondary tuner is not used
        std::lock_guard<std::mutex> lk(propMutex);
        props[e.property] = e.value;
        ++propSeq[e.property];
        propCv.notify_all();
    }

    bool handle(NetLink& link, ws::Message& m) {
        switch (m.op) {
            case ws::kText: {
                sc::Envelope e;
                if (sc::parseMessage(std::string(m.payload.begin(), m.payload.end()), e)) { notePush(e); }
                return true;  // a text message that is not an envelope is ignored, not fatal
            }
            case ws::kBinary: {
                const sc::BinaryFrame f = sc::classifyBinary(m.payload.data(), m.payload.size());
                if (f.kind == sc::BinaryKind::Iq && !f.secondary) {
                    const std::size_t pairs =
                        sc::decodeIq16(m.payload.data() + f.payloadOffset, m.payload.size() - f.payloadOffset, iq);
                    if (pairs > 0) { link.pushSamples(iq.data(), pairs); }
                } else {
                    binaryOther.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }
            case ws::kPing:
                sendMessage(ws::kPong, m.payload.data(), m.payload.size());
                return true;
            case ws::kPong: return true;
            case ws::kClose:
                serverClosed.store(true);
                link.fault("SDRconnect closed the connection", "it sent a WebSocket close");
                return false;
            default: return true;
        }
    }

    bool pump(NetLink& link) {
        std::size_t got = 0;
        if (!t->recvSome(buf.data(), buf.size(), got)) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            const char* err = t->lastError();
            if (err != nullptr && std::strstr(err, "timed out") != nullptr) {
                // Idle is silence by design; a streaming source silent for several slices has
                // lost its server.
                if (!link.streaming.load(std::memory_order_relaxed)) {
                    silentTicks = 0;  // idle slices are not silence: only consecutive streaming ones count
                } else if (++silentTicks >= silentLimit) {
                    link.fault("SDRconnect stopped sending", "no data arrived");
                    return false;
                }
                return true;
            }
            link.fault("the SDRconnect connection failed", err != nullptr ? err : "");
            return false;
        }
        silentTicks = 0;
        parser.feed(buf.data(), got);
        if (parser.failed()) {
            link.fault("SDRconnect sent a malformed WebSocket frame", parser.error());
            return false;
        }
        ws::Message m;
        while (parser.next(m)) {
            if (!handle(link, m)) { return false; }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

SdrConnectSource::~SdrConnectSource() { closeDevice(); }

bool SdrConnectSource::failOpen(const std::string& why) {
    setError(why);
    teardown();
    return false;
}

bool SdrConnectSource::open(const std::string& args) { return openImpl(args, nullptr); }

bool SdrConnectSource::openWithTransportForTest(const std::string& args, std::shared_ptr<iiod::Transport> t) {
    return openImpl(args, std::move(t));
}

bool SdrConnectSource::openImpl(const std::string& args, std::shared_ptr<iiod::Transport> injected) {
    closeDevice();
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::string addr = argValue(args, "sdrconnect");
    if (addr.empty()) { addr = "127.0.0.1"; }
    std::string host, err;
    std::uint16_t port = kSdrConnectDefaultPort;
    if (!parseHostPort(addr, kSdrConnectDefaultPort, host, port, err)) { setError(err); return false; }
    std::string path = argValue(args, "path");
    if (path.empty()) { path = "/"; }
    if (path[0] != '/' || path.find_first_of(" \r\n") != std::string::npos) {
        setError("the WebSocket path must start with / and hold no spaces");
        return false;
    }
    timeoutMs_ = 3000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        timeoutMs_ = static_cast<int>(v);
    }
    iqRateHz_ = 0.0;
    const std::string iqr = argValue(args, "iqrate");
    if (!iqr.empty()) {
        char* end = nullptr;
        const double v = std::strtod(iqr.c_str(), &end);
        if (end == iqr.c_str() || *end != '\0' || !std::isfinite(v) || v < 1000.0 || v > 1.0e9) {
            setError("the I/Q rate must be a number of samples per second from 1000 to 1000000000");
            return false;
        }
        iqRateHz_ = v;
    }
    const std::string serial = argValue(args, "serial"), mode = argValue(args, "mode"),
                      dname = argValue(args, "name"), index = argValue(args, "index");

    auto conn = std::make_shared<Conn>();
    conn->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    if (injected != nullptr) {
        conn->t = std::move(injected);
    } else {
        iiod::PeerWording w;
        w.name = "SDRconnect";
        w.unknownHostHint = "is the address right, and is SDRconnect running with its WebSocket API?";
        std::string cerr;
        // The handshake is read with the full timeout; the reader then uses short slices.
        std::unique_ptr<iiod::Transport> t = iiod::connectTcp(
            host, port, std::chrono::milliseconds(3000), std::chrono::milliseconds(kPollMs), cerr, w);
        if (t == nullptr) { setError(cerr); return false; }
        conn->t = std::move(t);
    }
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_ = conn;
    }

    // --- the opening handshake, on this thread ---------------------------------------------
    std::array<std::uint8_t, 16> nonce{};
    {
        std::random_device rd;
        for (auto& b : nonce) { b = static_cast<std::uint8_t>(rd()); }
    }
    const std::string key = ws::base64(nonce.data(), nonce.size());
    const std::string req = ws::handshakeRequest(host, port, path, nonce);
    if (!conn->t->sendAll(req.data(), req.size())) { return failOpen(std::string("could not send to SDRconnect: ") + conn->t->lastError()); }
    std::string head;
    std::vector<std::uint8_t> extra;
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
        std::vector<std::uint8_t> tmp(1024);
        bool done = false;
        while (!done) {
            std::size_t got = 0;
            if (!conn->t->recvSome(tmp.data(), tmp.size(), got)) {
                const char* e = conn->t->lastError();
                const bool timeout = e != nullptr && std::strstr(e, "timed out") != nullptr;
                if (timeout && std::chrono::steady_clock::now() < deadline) { continue; }
                return failOpen(timeout ? "SDRconnect did not answer the WebSocket handshake (receive timed out)"
                                        : std::string("the SDRconnect connection failed during the handshake: ") + (e ? e : ""));
            }
            head.append(reinterpret_cast<const char*>(tmp.data()), got);
            const std::size_t end = head.find("\r\n\r\n");
            if (end != std::string::npos) {
                const std::string rest = head.substr(end + 4);
                extra.assign(rest.begin(), rest.end());
                head.resize(end + 4);
                done = true;
            } else if (head.size() > kMaxHandshakeBytes) {
                return failOpen("SDRconnect's handshake reply is longer than any HTTP header should be");
            }
        }
    }
    const std::string bad = ws::checkHandshakeResponse(head, key);
    if (!bad.empty()) { return failOpen(bad); }
    if (!extra.empty()) { conn->parser.feed(extra.data(), extra.size()); }

    // --- the reader, then the requests (their replies come through the reader) ---------------
    std::shared_ptr<Conn> keep = conn;
    if (!beginReader([keep](NetLink& l) { return keep->pump(l); }, [keep]() { keep->t->shutdown(); }, keep)) {
        return failOpen(lastError());
    }
    std::string v;
    if (!getProperty("api_version", v)) {
        return failOpen("this is not an SDRconnect WebSocket server: it did not answer api_version (" +
                        std::string(lastError()) + ")");
    }
    if (!serial.empty()) {
        if (!sendRequest("selected_device_serial", "", mode.empty() ? serial : serial + ":" + mode)) { return failOpen(lastError()); }
    } else if (!dname.empty()) {
        if (!sendRequest("selected_device_name", "", mode.empty() ? dname : dname + ":" + mode)) { return failOpen(lastError()); }
    } else if (!index.empty()) {
        char* end = nullptr;
        const long n = std::strtol(index.c_str(), &end, 10);
        if (end == index.c_str() || *end != '\0' || n < 0 || n > 1000) { return failOpen("the device index is not a number"); }
        if (!sendRequest("selected_device", "", std::to_string(n))) { return failOpen(lastError()); }
    }
    // The state the receiver reads after an open. Each is a request and its answer; a server
    // that does not answer one of the optional ones still opens.
    std::string tmp;
    if (!getProperty("device_sample_rate", tmp)) { return failOpen("SDRconnect did not report its sample rate (" + std::string(lastError()) + ")"); }
    if (!getProperty("device_center_frequency", tmp)) { return failOpen("SDRconnect did not report its centre frequency (" + std::string(lastError()) + ")"); }
    setError("");
    antennas_.clear();
    devices_.clear();
    hasLna_ = false;
    if (getProperty("valid_antennas", tmp)) { antennas_ = sc::splitList(tmp); }
    if (getProperty("valid_devices", tmp)) { devices_ = sc::splitList(tmp); }
    getProperty("active_device", tmp);
    getProperty("active_antenna", tmp);
    std::string lo, hi, cur;
    if (getProperty("lna_state_min", lo) && getProperty("lna_state_max", hi)) {
        char* e1 = nullptr;
        char* e2 = nullptr;
        const long a = std::strtol(lo.c_str(), &e1, 10);
        const long b = std::strtol(hi.c_str(), &e2, 10);
        if (e1 != lo.c_str() && e2 != hi.c_str() && b >= a && b - a < 1000) {
            lnaMin_ = static_cast<int>(a);
            lnaMax_ = static_cast<int>(b);
            hasLna_ = true;
            getProperty("lna_state", cur);
        }
    }
    setError("");
    markOpen(true);
    setName("SDRconnect: " + (prop("active_device").empty() ? host : prop("active_device")));
    return true;
}

void SdrConnectSource::teardown() {
    if (conn_ != nullptr) {
        // A WebSocket close (code 1000, no reason), best effort.
        const std::uint8_t code[2] = {0x03, 0xE8};
        if (isOpen() || running()) { conn_->sendMessage(ws::kClose, code, 2); }
    }
    active_ = false;
    markOpen(false);
    endReader();
    if (conn_ != nullptr) {
        if (conn_->t) { conn_->t->close(); }
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_.reset();
    }
}

void SdrConnectSource::closeDevice() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (conn_ == nullptr && !isOpen()) { endReader(); return; }
    const std::string keep = lastError();
    if (conn_ != nullptr && active_) {
        sendRequest("iq_stream_enable", "", "false");
        sendRequest("device_stream_enable", "", "false");
    }
    teardown();
    if (!keep.empty()) { setError(keep); }
    antennas_.clear();
    devices_.clear();
    hasLna_ = false;
    setName("network radio: (not connected)");
}

bool SdrConnectSource::sendRequest(const std::string& eventType, const std::string& property,
                                   const std::string& value) {
    if (conn_ == nullptr) { setError("no SDRconnect connection is open"); return false; }
    const std::string text = sc::buildMessage(eventType, property, value);
    if (!conn_->sendMessage(ws::kText, reinterpret_cast<const std::uint8_t*>(text.data()), text.size())) {
        setError(std::string("could not send to SDRconnect: ") + conn_->t->lastError());
        return false;
    }
    return true;
}

bool SdrConnectSource::getProperty(const std::string& name, std::string& value) {
    if (conn_ == nullptr) { setError("no SDRconnect connection is open"); return false; }
    std::uint64_t before = 0;
    {
        std::lock_guard<std::mutex> lk(conn_->propMutex);
        before = conn_->propSeq[name];
    }
    if (!sendRequest("get_property", name, "")) { return false; }
    std::unique_lock<std::mutex> lk(conn_->propMutex);
    const bool ok = conn_->propCv.wait_for(lk, std::chrono::milliseconds(timeoutMs_), [&] {
        return conn_->propSeq[name] != before || faulted();
    });
    if (!ok || conn_->propSeq[name] == before) {
        setError("SDRconnect did not answer for " + name + " (receive timed out)");
        return false;
    }
    value = conn_->props[name];
    return true;
}

bool SdrConnectSource::setProperty(const std::string& name, const std::string& value) {
    if (!sendRequest("set_property", name, value)) { return false; }
    std::string back;
    if (!getProperty(name, back)) { return false; }  // the read-back is the acknowledgement
    return true;
}

std::string SdrConnectSource::prop(const std::string& name) const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c == nullptr) { return std::string(); }
    std::lock_guard<std::mutex> lk(c->propMutex);
    const auto it = c->props.find(name);
    return it == c->props.end() ? std::string() : it->second;
}

bool SdrConnectSource::start() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("start() called with no SDRconnect open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    if (!sendRequest("iq_stream_enable", "", "true") || !sendRequest("device_stream_enable", "", "true")) { return false; }
    active_ = true;
    return startKeeping();
}

void SdrConnectSource::stop() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    stopKeeping();
    if (active_ && conn_ != nullptr) {
        sendRequest("iq_stream_enable", "", "false");
        sendRequest("device_stream_enable", "", "false");
    }
    active_ = false;
}

double SdrConnectSource::sampleRateHz() const {
    if (iqRateHz_ > 0.0) { return iqRateHz_; }
    const std::string v = prop("device_sample_rate");
    return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
}

bool SdrConnectSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no SDRconnect open"); return false; }
    if (!(hz > 0.0) || !std::isfinite(hz) || hz > 1e9) { setError("setSampleRateHz() requires a positive rate"); return false; }
    if (iqRateHz_ > 0.0) {
        if (hz == iqRateHz_) { return true; }
        setError("the I/Q rate is fixed by the iqrate setting");
        return false;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.0f", hz);
    return setProperty("device_sample_rate", buf);
}

double SdrConnectSource::centerFrequencyHz() const {
    const std::string v = prop("device_center_frequency");
    return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
}

bool SdrConnectSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setCenterFrequencyHz() called with no SDRconnect open"); return false; }
    if (!(hz >= 0.0) || !std::isfinite(hz) || hz > 1e13) { setError("setCenterFrequencyHz() requires a frequency"); return false; }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.0f", hz);  // Uint64 Hz, as the document says
    return setProperty("device_center_frequency", buf);
}

std::vector<GainInfo> SdrConnectSource::gains() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::vector<GainInfo> v;
    if (hasLna_) {
        GainInfo g;
        g.name = "LNA state";
        g.minDb = lnaMin_;
        g.maxDb = lnaMax_;
        g.stepDb = 1.0;
        g.unit = GainUnit::Steps;  // the document gives states, not decibels; none is invented
        v.push_back(g);
    }
    return v;
}

bool SdrConnectSource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() || !hasLna_ || name != "LNA state") { setError("this radio has no gain called " + name); return false; }
    if (!std::isfinite(db)) { setError("setGainDb() requires a number"); return false; }
    const long v = std::min<long>(lnaMax_, std::max<long>(lnaMin_, std::lround(db)));
    return setProperty("lna_state", std::to_string(v));
}

double SdrConnectSource::gainDb(const std::string& name) const {
    if (name != "LNA state") { return 0.0; }
    const std::string v = prop("lna_state");
    return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
}

std::vector<std::string> SdrConnectSource::antennas() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return antennas_;
}

bool SdrConnectSource::setAntenna(const std::string& name) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { return false; }
    if (std::find(antennas_.begin(), antennas_.end(), name) == antennas_.end()) {
        setError("this radio has no antenna called " + name);
        return false;
    }
    return setProperty("active_antenna", name);
}

std::string SdrConnectSource::antenna() const { return prop("active_antenna"); }

std::vector<double> SdrConnectSource::supportedSampleRatesHz() const {
    const double r = sampleRateHz();
    return r > 0.0 ? std::vector<double>{r} : std::vector<double>{};
}

std::string SdrConnectSource::apiVersion() const { return prop("api_version"); }
std::string SdrConnectSource::activeDevice() const { return prop("active_device"); }

std::vector<std::string> SdrConnectSource::validDevices() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return devices_;
}

}  // namespace cascade::source
