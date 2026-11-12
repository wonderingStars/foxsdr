// raw_iq_net_source.cpp - see raw_iq_net_source.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/raw_iq_net_source.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace cascade::source {

namespace rawiq {

namespace {

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool parseDouble(const std::string& s, double& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) { return false; }
    out = v;
    return true;
}

}  // namespace

bool parseHeader(const unsigned char* p, std::size_t n, Header& out) {
    if (p == nullptr || n < kHeaderBytes) { return false; }
    if (std::memcmp(p, "FXIQ", 4) != 0) { return false; }
    const unsigned version = static_cast<unsigned>(p[4]) | (static_cast<unsigned>(p[5]) << 8);
    const unsigned format = static_cast<unsigned>(p[6]) | (static_cast<unsigned>(p[7]) << 8);
    if (version != 1 || format < 1 || format > 3) { return false; }
    const std::uint32_t rate = le32(p + 8);
    if (rate < kMinRateHz || rate > kMaxRateHz) { return false; }
    const std::uint64_t bits = static_cast<std::uint64_t>(le32(p + 12)) | (static_cast<std::uint64_t>(le32(p + 16)) << 32);
    const double centre = std::bit_cast<double>(bits);
    if (!std::isfinite(centre) || std::fabs(centre) > 1e13) { return false; }
    out.format = static_cast<Format>(format);
    out.rateHz = rate;
    out.centreHz = centre;
    out.sequence = le32(p + 20);
    return true;
}

std::size_t pairBytes(Format f) {
    switch (f) {
        case Format::U8: return 2;
        case Format::S16: return 4;
        case Format::F32: return 8;
        default: return 0;
    }
}

void decode(Format f, const unsigned char* b, std::size_t pairs, std::complex<float>* out) {
    switch (f) {
        case Format::U8:
            for (std::size_t i = 0; i < pairs; ++i) { out[i] = {(static_cast<float>(b[2 * i]) - 127.5f) / 127.5f, (static_cast<float>(b[2 * i + 1]) - 127.5f) / 127.5f}; }
            break;
        case Format::S16:
            for (std::size_t i = 0; i < pairs; ++i) {
                const std::int16_t re = static_cast<std::int16_t>(static_cast<unsigned>(b[4 * i]) | (static_cast<unsigned>(b[4 * i + 1]) << 8));
                const std::int16_t im = static_cast<std::int16_t>(static_cast<unsigned>(b[4 * i + 2]) | (static_cast<unsigned>(b[4 * i + 3]) << 8));
                out[i] = {static_cast<float>(re) / 32767.0f, static_cast<float>(im) / 32767.0f};
            }
            break;
        case Format::F32:
            for (std::size_t i = 0; i < pairs; ++i) {
                const float re = std::bit_cast<float>(le32(b + 8 * i)), im = std::bit_cast<float>(le32(b + 8 * i + 4));
                out[i] = {std::isfinite(re) ? re : 0.0f, std::isfinite(im) ? im : 0.0f};
            }
            break;
        default:
            for (std::size_t i = 0; i < pairs; ++i) { out[i] = {0.0f, 0.0f}; }
            break;
    }
}

bool parseSpec(const std::string& argsIn, Spec& out, std::string& error) {
    Spec s;
    std::string a = argsIn;
    const std::size_t q = a.find('?');
    std::string query;
    if (q != std::string::npos) {
        query = a.substr(q + 1);
        a.resize(q);
    }
    if (a.rfind("tcp://", 0) == 0) {
        a.erase(0, 6);
    } else if (a.rfind("udp://", 0) == 0) {
        s.udp = true;
        a.erase(0, 6);
    }
    const std::size_t colon = a.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= a.size()) {
        error = "the address is host:port (for example 192.168.1.20:5555, or udp://127.0.0.1:5555)";
        return false;
    }
    s.host = a.substr(0, colon);
    if (s.host.size() > 2 && s.host.front() == '[' && s.host.back() == ']') { s.host = s.host.substr(1, s.host.size() - 2); }
    const std::string portText = a.substr(colon + 1);
    char* end = nullptr;
    const long port = std::strtol(portText.c_str(), &end, 10);
    if (end == portText.c_str() || *end != '\0' || port < 1 || port > 65535) {
        error = "the port is a number from 1 to 65535";
        return false;
    }
    s.port = static_cast<std::uint16_t>(port);
    bool haveRate = false;
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) { amp = query.size(); }
        const std::string kv = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (kv.empty()) { continue; }
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = "\"" + kv + "\" is not key=value";
            return false;
        }
        const std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
        if (key == "fmt") {
            if (val == "u8") { s.format = Format::U8; }
            else if (val == "s16") { s.format = Format::S16; }
            else if (val == "f32") { s.format = Format::F32; }
            else {
                error = "fmt is u8, s16 or f32";
                return false;
            }
        } else if (key == "rate") {
            double r = 0.0;
            if (!parseDouble(val, r) || r < kMinRateHz || r > kMaxRateHz) {
                error = "rate is a sample rate from 1000 to 100000000 Hz";
                return false;
            }
            s.rateHz = r;
            haveRate = true;
        } else if (key == "freq") {
            double f = 0.0;
            if (!parseDouble(val, f) || std::fabs(f) > 1e13) {
                error = "freq is a frequency in Hz";
                return false;
            }
            s.centreHz = f;
        } else if (key == "hdr") {
            if (val != "0" && val != "1") {
                error = "hdr is 0 or 1";
                return false;
            }
            s.header = val == "1";
        } else if (key == "timeout") {
            double t = 0.0;
            if (!parseDouble(val, t) || t < 200.0 || t > 60000.0) {
                error = "timeout is from 200 to 60000 milliseconds";
                return false;
            }
            s.timeoutMs = static_cast<int>(t);
        } else {
            error = "\"" + key + "\" is not a setting of this source (fmt, rate, freq, hdr, timeout)";
            return false;
        }
    }
    if (!s.header && !haveRate) {
        error = "a stream without a header needs its sample rate: add ?rate=<Hz> (and ?fmt=u8|s16|f32)";
        return false;
    }
    out = s;
    return true;
}

}  // namespace rawiq

namespace {

constexpr int kPollMs = 200;
bool timedOut(const char* err) { return err != nullptr && std::strstr(err, "timed out") != nullptr; }

}  // namespace

// --- the reader --------------------------------------------------------------------------------------------------------------------------------

struct RawIqNetSource::Reader {
    std::shared_ptr<iiod::Transport> t;
    bool datagram = false;
    bool header = false;
    rawiq::Format format = rawiq::Format::F32;
    std::uint32_t rateHz = 0;
    int silentLimit = 15;
    int silentTicks = 0;
    std::vector<unsigned char> buf = std::vector<unsigned char>(1u << 16);
    std::size_t carry = 0;              // bytes at the start of buf that did not make a whole sample last time (a stream only)
    std::vector<std::complex<float>> out;
    bool haveSeq = false;
    std::uint32_t expectSeq = 0;
    std::atomic<std::uint64_t>* stray = nullptr;
    // bytes already received that belong to the stream's samples (the first datagram's, left by open()):
    std::vector<unsigned char> first;

    // One unit: a receive, then whatever whole samples it holds.
    bool pump(NetLink& link) {
        std::size_t got = 0;
        if (!first.empty()) {
            std::memcpy(buf.data() + carry, first.data(), first.size());
            got = first.size();
            first.clear();
        } else {
            const bool ok = t->recvSome(buf.data() + carry, buf.size() - carry, got);
            if (!ok) {
                if (!link.run.load(std::memory_order_relaxed)) { return false; }   // we are closing
                if (timedOut(t->lastError())) {
                    if (link.streaming.load(std::memory_order_relaxed) && ++silentTicks >= silentLimit) {
                        link.fault("the I/Q stream stopped", "no samples arrived for " + std::to_string(silentLimit * kPollMs / 1000.0).substr(0, 4) + " s");
                        return false;
                    }
                    if (!link.streaming.load(std::memory_order_relaxed)) { silentTicks = 0; }
                    return true;
                }
                link.fault("the I/Q stream connection failed", t->lastError());
                return false;
            }
            silentTicks = 0;
        }
        std::size_t have = carry + got;
        std::size_t off = 0;
        if (datagram && header) {
            rawiq::Header h;
            if (!rawiq::parseHeader(buf.data(), have, h) || h.format != format || h.rateHz != rateHz) {
                stray->fetch_add(1, std::memory_order_relaxed);   // not a FXIQ datagram of this stream: dropped, counted, never fatal
                carry = 0;
                return true;
            }
            if (haveSeq) {
                if (h.sequence != expectSeq) {
                    const std::uint32_t gap = h.sequence - expectSeq;
                    if (gap < 0x80000000u) { link.dropped.fetch_add(gap, std::memory_order_relaxed); }   // datagrams missing
                    else { stray->fetch_add(1, std::memory_order_relaxed); carry = 0; return true; }      // a repeat or a late one
                }
            }
            haveSeq = true;
            expectSeq = h.sequence + 1;
            off = rawiq::kHeaderBytes;
        }
        const std::size_t pb = rawiq::pairBytes(format);
        const std::size_t pairs = (have - off) / pb;
        if (pairs > 0) {
            out.resize(pairs);
            rawiq::decode(format, buf.data() + off, pairs, out.data());
            link.pushSamples(out.data(), pairs);
        }
        const std::size_t leftover = have - off - pairs * pb;
        if (datagram) {
            carry = 0;   // an odd tail of a datagram is not continued by the next one
        } else {
            if (leftover > 0) { std::memmove(buf.data(), buf.data() + off + pairs * pb, leftover); }
            carry = leftover;
        }
        return true;
    }
};

// --- the source ----------------------------------------------------------------------------------------------------------------------------------

RawIqNetSource::~RawIqNetSource() { teardown(); }

bool RawIqNetSource::open(const std::string& args) { return openImpl(args, nullptr); }
bool RawIqNetSource::openWithTransportForTest(const std::string& args, std::shared_ptr<iiod::Transport> t) { return openImpl(args, std::move(t)); }

bool RawIqNetSource::failOpen(const std::string& why) {
    setError(why);
    teardown();
    return false;
}

bool RawIqNetSource::openImpl(const std::string& args, std::shared_ptr<iiod::Transport> injected) {
    teardown();
    rawiq::Spec spec;
    std::string error;
    // (the saved settings and the engine call it "rawiq=<address>")
    if (!rawiq::parseSpec(args.rfind("rawiq=", 0) == 0 ? args.substr(6) : args, spec, error)) {
        setError(error);
        return false;
    }
    std::shared_ptr<iiod::Transport> t = std::move(injected);
    if (!t) {
        if (spec.udp) {
            std::string e;
            std::unique_ptr<UdpTransport> u = UdpTransport::bindLocal(spec.host, spec.port, std::chrono::milliseconds(kPollMs), e);
            if (!u) {
                setError("cannot listen for I/Q on " + spec.host + ":" + std::to_string(spec.port) + ": " + e);
                return false;
            }
            t = std::move(u);
        } else {
            std::string e;
            iiod::PeerWording w;
            w.name = "the I/Q stream";
            std::unique_ptr<iiod::Transport> c = iiod::connectTcp(spec.host, spec.port, std::chrono::milliseconds(3000), std::chrono::milliseconds(kPollMs), e, w);
            if (!c) {
                setError(e);
                return false;
            }
            t = std::move(c);
        }
    }
    auto r = std::make_shared<Reader>();
    r->t = t;
    r->datagram = spec.udp;
    r->header = spec.header;
    r->format = spec.format;
    r->stray = &stray_;
    r->silentLimit = std::max(3, spec.timeoutMs / kPollMs);
    double rate = spec.rateHz, centre = spec.centreHz;
    rawiq::Header first{};
    if (spec.header) {
        // The first header, waited for (bounded): TCP sends it once at the start of the connection, UDP at the start of every datagram.
        std::vector<unsigned char> hb;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeoutMs);
        bool have = false;
        std::vector<unsigned char> chunk(1u << 16);
        while (!have && std::chrono::steady_clock::now() < deadline) {
            std::size_t got = 0;
            if (!t->recvSome(chunk.data(), chunk.size(), got)) {
                if (timedOut(t->lastError())) { continue; }
                return failOpen(std::string("the I/Q stream broke before its header: ") + t->lastError());
            }
            if (spec.udp) {
                if (rawiq::parseHeader(chunk.data(), got, first)) {
                    have = true;
                    r->first.assign(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));   // its samples are used: pump() reads them as the first datagram
                }
            } else {
                hb.insert(hb.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
                if (hb.size() >= rawiq::kHeaderBytes) {
                    if (!rawiq::parseHeader(hb.data(), hb.size(), first)) { return failOpen("what arrived is not a FoxSDR I/Q stream header (FXIQ, version 1); drop hdr=1 for a headerless stream"); }
                    have = true;
                    // samples that arrived with the header
                    const std::size_t extra = hb.size() - rawiq::kHeaderBytes;
                    if (extra > 0) { r->first.assign(hb.begin() + static_cast<std::ptrdiff_t>(rawiq::kHeaderBytes), hb.end()); }
                }
            }
        }
        if (!have) { return failOpen("no I/Q stream header arrived in " + std::to_string(spec.timeoutMs) + " ms"); }
        r->format = first.format;
        r->rateHz = first.rateHz;
        rate = first.rateHz;
        centre = first.centreHz;
        if (spec.udp) { r->haveSeq = true; r->expectSeq = first.sequence; }   // so the first datagram's own number is not read as a gap
    } else {
        r->rateHz = static_cast<std::uint32_t>(spec.rateHz);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        spec_ = spec;
        header_ = first;
        rateHz_ = rate;
        centreHz_ = centre;
        declaredCentre_ = spec.header || spec.centreHz != 0.0;
        transport_ = t;
        reader_ = r;
    }
    stray_.store(0, std::memory_order_relaxed);
    setName("raw I/Q stream: " + spec.host + ":" + std::to_string(spec.port));
    std::shared_ptr<Reader> keep = r;
    if (!beginReader([keep](NetLink& l) { return keep->pump(l); }, [t]() { t->shutdown(); }, keep)) { return failOpen("the I/Q reader could not be started"); }
    markOpen(true);
    return true;
}

void RawIqNetSource::teardown() {
    endReader();
    std::shared_ptr<iiod::Transport> t;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        t = std::move(transport_);
        reader_.reset();
    }
    if (t) { t->close(); }
    markOpen(false);
}

void RawIqNetSource::closeDevice() {
    stop();
    teardown();
}

bool RawIqNetSource::start() {
    if (!isOpen()) {
        setError("start() before a successful open()");
        return false;
    }
    if (!startKeeping()) { return false; }
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void RawIqNetSource::stop() {
    stopKeeping();
    running_.store(false, std::memory_order_relaxed);
}

double RawIqNetSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rateHz_;
}
bool RawIqNetSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    return hz == rateHz_;   // the sender decides: asking for the rate it already has is not a failure
}
double RawIqNetSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return centreHz_;
}
bool RawIqNetSource::setCenterFrequencyHz(double hz) {
    if (!std::isfinite(hz)) { return false; }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // A stream has no tuner. When its sender said where it is (a header's centre, or freq=) that is the truth and a retune cannot change it: the same number is accepted, another refused with the
        // sentence. When it said nothing the number is a label the user may set.
        if (declaredCentre_ && hz != centreHz_) {
            lastCentreMsg_ = "this stream is at " + std::to_string(centreHz_ / 1e6) + " MHz as its sender says; it has no tuner";
        } else {
            centreHz_ = hz;
            return true;
        }
    }
    setError(lastCentreMsg_);
    return false;
}
std::vector<double> RawIqNetSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rateHz_ > 0.0 ? std::vector<double>{rateHz_} : std::vector<double>{};
}

}  // namespace cascade::source
