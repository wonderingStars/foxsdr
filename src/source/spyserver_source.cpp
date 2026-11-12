// spyserver_source.cpp - see spyserver_source.hpp and spyserver_protocol.hpp (provenance, licences, hypotheses).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/spyserver_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "source/iiod_client.hpp"

namespace cascade::source {

namespace sp = spyserver;

namespace {

constexpr int kPollMs = 200;  // the transport's receive bound: a close is noticed within one slice

bool timedOut(const char* err) { return err != nullptr && std::strstr(err, "timed out") != nullptr; }

}  // namespace

// ---------------------------------------------------------------------------------------
// the connection, shared with the reader thread
// ---------------------------------------------------------------------------------------

struct SpyServerSource::Session {
    std::unique_ptr<iiod::Transport> tr;
    sp::MessageAssembler assembler;
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(65536);
    std::vector<std::complex<float>> pairs;
    std::vector<std::uint8_t> body;

    // Read by the control thread, written by the reader (and by open() before the reader exists).
    std::atomic<bool> gotInfo{false}, gotSync{false}, canControl{false}, deviceLost{false};
    std::atomic<std::uint32_t> syncGain{0}, syncIqCenter{0}, syncMinCenter{0}, syncMaxCenter{0}, deviceType{0};
    std::atomic<std::uint64_t> others{0}, gaps{0};
    std::uint32_t lastSeq = 0xFFFFFFFFu;
    bool haveSeq = false;
    int silentTicks = 0, silentLimit = 15;
    sp::DeviceInfo info;

    // One message. `link` is null during the handshake (no samples are expected then: they are counted and dropped).
    // Returns an empty string, or the sentence that makes the stream a fault.
    std::string handle(const sp::MessageHeader& h, const std::vector<std::uint8_t>& b, NetLink* link) {
        switch (h.type) {
            case sp::kMsgDeviceInfo: {
                sp::DeviceInfo d;
                if (!sp::parseDeviceInfo(b.data(), b.size(), d)) { return "a device description shorter than 48 bytes"; }
                info = d;
                deviceType.store(d.deviceType);
                if (d.deviceType == sp::kDeviceInvalid) { deviceLost.store(true); }
                gotInfo.store(true);
                return {};
            }
            case sp::kMsgClientSync: {
                sp::ClientSync s;
                if (!sp::parseClientSync(b.data(), b.size(), s)) { return "a synchronisation message shorter than 36 bytes"; }
                canControl.store(s.canControl != 0);
                syncGain.store(s.gain);
                syncIqCenter.store(s.iqCenter);
                syncMinCenter.store(s.minIqCenter);
                syncMaxCenter.store(s.maxIqCenter);
                gotSync.store(true);
                return {};
            }
            case sp::kMsgUint8Iq:
            case sp::kMsgInt16Iq:
            case sp::kMsgFloatIq: {
                if (h.stream == sp::kStreamIq) {
                    if (haveSeq) { gaps.fetch_add(static_cast<std::uint32_t>(h.sequence - lastSeq - 1u), std::memory_order_relaxed); }
                    lastSeq = h.sequence;
                    haveSeq = true;
                }
                const long n = sp::convertIq(h.type, b.data(), b.size(), pairs);
                if (n < 0) { return "an I/Q message that is not a whole number of samples"; }
                if (link != nullptr && n > 0) { link->pushSamples(pairs.data(), static_cast<std::size_t>(n)); }
                return {};
            }
            default:
                others.fetch_add(1, std::memory_order_relaxed);  // pong, setting read-back, 24-bit I/Q, audio, FFT, unknown
                return {};
        }
    }

    // Feeds received bytes and handles every whole message. Empty string, or the sentence that is the fault.
    std::string absorb(const std::uint8_t* p, std::size_t n, NetLink* link) {
        assembler.feed(p, n);
        sp::MessageHeader h;
        std::string err;
        for (;;) {
            const auto st = assembler.next(h, body, err);
            if (st == sp::MessageAssembler::Status::NeedMore) { return {}; }
            if (st == sp::MessageAssembler::Status::Error) { return err; }
            err = handle(h, body, link);
            if (!err.empty()) { return err; }
        }
    }

    bool pump(NetLink& link) {
        std::size_t got = 0;
        if (!tr->recvSome(buf.data(), buf.size(), got)) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }  // we are closing
            if (timedOut(tr->lastError())) {
                if (!link.streaming.load(std::memory_order_relaxed)) {
                    silentTicks = 0;  // idle slices are not silence
                } else if (++silentTicks >= silentLimit) {
                    link.fault("the SpyServer stopped sending", "no I/Q data arrived");
                    return false;
                }
                return true;
            }
            link.fault("the SpyServer connection failed", tr->lastError());
            return false;
        }
        const std::string err = absorb(buf.data(), got, &link);
        if (!err.empty()) {
            link.fault("the SpyServer sent something this client cannot read", err);
            return false;
        }
        if (deviceLost.load()) {
            link.fault("the SpyServer lost its radio", "the server now reports no device");
            return false;
        }
        silentTicks = 0;
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

SpyServerSource::~SpyServerSource() { closeDevice(); }

bool SpyServerSource::failOpen(const std::string& why) {
    setError(why);
    session_.reset();
    return false;
}

bool SpyServerSource::sendSet(std::uint32_t setting, std::uint32_t value, const char* what) {
    if (!session_ || !session_->tr) { setError(std::string(what) + ": no SpyServer is open"); return false; }
    const std::vector<std::uint8_t> cmd = sp::encodeSet(setting, value);
    if (!session_->tr->sendAll(cmd.data(), cmd.size())) {
        setError(std::string(what) + ": " + session_->tr->lastError());
        return false;
    }
    return true;
}

bool SpyServerSource::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(ctlMutex_);
    const std::string addr = argValue(args, "spyserver");
    if (addr.empty()) {
        return failOpen("type the address of the SpyServer to connect to (host or host:port); there is no default server");
    }
    std::string host, err;
    std::uint16_t port = sp::kDefaultPort;
    if (!parseHostPort(addr, sp::kDefaultPort, host, port, err)) { return failOpen(err); }
    timeoutMs_ = 3000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        char* e = nullptr;
        const long v = std::strtol(to.c_str(), &e, 10);
        if (e == to.c_str() || *e != '\0' || v < 100 || v > 60000) { return failOpen("the timeout must be 100 to 60000 ms"); }
        timeoutMs_ = static_cast<int>(v);
    }
    format_ = sp::kFmtInt16;
    const std::string fmt = argValue(args, "format");
    if (fmt == "uint8") { format_ = sp::kFmtUint8; }
    else if (fmt == "float") { format_ = sp::kFmtFloat; }
    else if (!fmt.empty() && fmt != "int16") { return failOpen("the format must be int16, uint8 or float"); }
    double wantRate = 0.0;
    const std::string rt = argValue(args, "rate");
    if (!rt.empty()) {
        char* e = nullptr;
        wantRate = std::strtod(rt.c_str(), &e);
        if (e == rt.c_str() || *e != '\0' || !(wantRate >= 1.0) || wantRate > 1e9) { return failOpen("the rate must be 1 to 1000000000 samples per second"); }
    }

    iiod::PeerWording w;
    w.name = "the SpyServer";
    w.unknownHostHint = "is the address right?";
    w.unreachableHint = "is the SpyServer running, and is its port open?";
    std::unique_ptr<iiod::Transport> t =
        iiod::connectTcp(host, port, std::chrono::milliseconds(timeoutMs_), std::chrono::milliseconds(kPollMs), err, w);
    if (t == nullptr) { return failOpen(err); }
    auto s = std::make_shared<Session>();
    s->tr = std::move(t);
    s->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    session_ = s;

    const std::vector<std::uint8_t> hello = sp::encodeHello();
    if (!s->tr->sendAll(hello.data(), hello.size())) { return failOpen(std::string("could not greet the SpyServer: ") + s->tr->lastError()); }

    // Wait for the two description messages.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
    bool anyBytes = false;
    while (!(s->gotInfo.load() && s->gotSync.load())) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return failOpen("the SpyServer did not describe its radio within " + std::to_string(timeoutMs_) + " ms");
        }
        std::size_t got = 0;
        if (!s->tr->recvSome(s->buf.data(), s->buf.size(), got)) {
            if (timedOut(s->tr->lastError())) { continue; }
            return failOpen(anyBytes ? std::string("the SpyServer connection failed while it was describing its radio: ") + s->tr->lastError()
                                     : std::string("the SpyServer closed the connection without describing a radio - it may be full, "
                                                   "or have no radio free (") + s->tr->lastError() + ")");
        }
        anyBytes = true;
        const std::string e = s->absorb(s->buf.data(), got, nullptr);
        if (!e.empty()) { return failOpen("the SpyServer sent something this client cannot read: " + e); }
    }
    info_ = s->info;
    if (info_.deviceType == sp::kDeviceInvalid) {
        return failOpen("the SpyServer is up but reports no radio (is the dongle plugged into the server?)");
    }
    if (!s->canControl.load()) {
        return failOpen("the SpyServer did not give this client control of the radio (another client may hold it)");
    }
    std::uint32_t stage = 0;
    double rate = 0.0;
    const double ask = wantRate > 0.0 ? wantRate : static_cast<double>(info_.maxSampleRate);
    if (!sp::stageForRate(info_, ask, stage, rate)) { return failOpen("the SpyServer's radio reports no sample rates"); }
    stage_ = stage;
    rateHz_ = rate;
    host_ = host;

    if (!sendSet(sp::kSetStreamingMode, sp::kModeIqOnly, "setting the stream mode") || !sendSet(sp::kSetIqFormat, format_, "setting the I/Q format") ||
        !sendSet(sp::kSetIqDecimation, stage_, "setting the sample rate")) {
        const std::string why = lastError();
        return failOpen(why);
    }

    std::shared_ptr<Session> keep = s;
    if (!beginReader([keep](NetLink& l) { return keep->pump(l); }, [keep]() { keep->tr->shutdown(); }, keep)) {
        return failOpen("could not start the SpyServer reader");
    }
    setError("");
    markOpen(true);
    setName(std::string("SpyServer: ") + sp::deviceName(info_.deviceType) + " (" + host_ + ")");
    return true;
}

void SpyServerSource::closeDevice() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() && !session_) { endReader(); return; }
    const std::string keep = lastError();
    if (active_ && session_) { sendSet(sp::kSetStreamingEnabled, 0, "stopping the stream"); }
    active_ = false;
    endReader();  // wakes the receive, joins with a bound, or abandons
    if (session_ && session_->tr) { session_->tr->close(); }
    session_.reset();
    markOpen(false);
    if (!keep.empty()) { setError(keep); }
    setName("network radio: (not connected)");
}

bool SpyServerSource::start() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() || !session_) { setError("start() called with no SpyServer open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    if (!startKeeping()) { return false; }
    if (!sendSet(sp::kSetStreamingEnabled, 1, "starting the stream")) { stopKeeping(); return false; }
    active_ = true;
    return true;
}

void SpyServerSource::stop() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    const bool was = active_;
    active_ = false;
    NetSampleSource::stop();
    if (was && session_ && !faulted()) { sendSet(sp::kSetStreamingEnabled, 0, "stopping the stream"); }
}

double SpyServerSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return rateHz_;
}

std::vector<double> SpyServerSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return isOpen() ? sp::sampleRatesFor(info_) : std::vector<double>{};
}

bool SpyServerSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no SpyServer open"); return false; }
    if (!std::isfinite(hz) || hz <= 0.0) { setError("the sample rate must be a positive number"); return false; }
    std::uint32_t stage = 0;
    double rate = 0.0;
    if (!sp::stageForRate(info_, hz, stage, rate)) { setError("the SpyServer's radio reports no sample rates"); return false; }
    if (!sendSet(sp::kSetIqDecimation, stage, "setting the sample rate")) { return false; }
    stage_ = stage;
    rateHz_ = rate;
    return true;
}

double SpyServerSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!session_) { return 0.0; }
    return static_cast<double>(session_->syncIqCenter.load());
}

bool SpyServerSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() || !session_) { setError("setCenterFrequencyHz() called with no SpyServer open"); return false; }
    if (!std::isfinite(hz) || hz < 0.0 || hz > 4294967295.0) { setError("the frequency must be 0 to 4294967295 Hz"); return false; }
    const double lo = static_cast<double>(session_->syncMinCenter.load()), hi = static_cast<double>(session_->syncMaxCenter.load());
    if (hi >= lo && hi > 0.0 && (hz < lo || hz > hi)) {
        setError("the SpyServer's radio tunes " + std::to_string(static_cast<long long>(lo)) + " to " + std::to_string(static_cast<long long>(hi)) +
                 " Hz; " + std::to_string(static_cast<long long>(hz)) + " Hz is outside that");
        return false;
    }
    const std::uint32_t v = static_cast<std::uint32_t>(std::llround(hz));
    if (!sendSet(sp::kSetIqFrequency, v, "tuning")) { return false; }
    session_->syncIqCenter.store(v);  // what was asked for, until the server's next synchronisation says otherwise
    return true;
}

bool SpyServerSource::frequencyRangeHz(double& lo, double& hi) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!session_) { return false; }
    const double a = static_cast<double>(session_->syncMinCenter.load()), b = static_cast<double>(session_->syncMaxCenter.load());
    if (!(b >= a) || b <= 0.0) { return false; }
    lo = a;
    hi = b;
    return true;
}

std::vector<GainInfo> SpyServerSource::gains() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() || info_.maxGainIndex == 0) { return {}; }
    GainInfo g;
    g.name = "Gain";
    g.minDb = 0.0;
    g.maxDb = static_cast<double>(info_.maxGainIndex);
    g.stepDb = 1.0;
    g.unit = GainUnit::Steps;
    return {g};
}

bool SpyServerSource::setGainDb(const std::string& name, double v) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() || !session_) { setError("setGainDb() called with no SpyServer open"); return false; }
    if (name != "Gain" || info_.maxGainIndex == 0) { setError("the SpyServer has no gain called \"" + name + "\""); return false; }
    if (!std::isfinite(v)) { setError("the gain must be a number"); return false; }
    const double c = std::min(std::max(std::round(v), 0.0), static_cast<double>(info_.maxGainIndex));  // clamped, as the contract says
    if (!sendSet(sp::kSetGain, static_cast<std::uint32_t>(c), "setting the gain")) { return false; }
    session_->syncGain.store(static_cast<std::uint32_t>(c));
    return true;
}

double SpyServerSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!session_ || name != "Gain") { return 0.0; }
    return static_cast<double>(session_->syncGain.load());
}

std::string SpyServerSource::deviceTypeName() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return isOpen() ? sp::deviceName(info_.deviceType) : std::string();
}
std::uint64_t SpyServerSource::otherMessages() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return session_ ? session_->others.load() : 0;
}
std::uint64_t SpyServerSource::sequenceGaps() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return session_ ? session_->gaps.load() : 0;
}

}  // namespace cascade::source
