// iq_out.cpp - see iq_out.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/iq_out.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cascade::net {
namespace {

constexpr float kFloatMax = 3.4028234663852886e38f;

bool hostIsLittleEndian() noexcept {
    const std::uint32_t one = 1;
    std::uint8_t b = 0;
    std::memcpy(&b, &one, 1);
    return b == 1;
}

inline float clean(float x) noexcept { return (std::fabs(x) <= kFloatMax) ? x : 0.0f; }  // NaN and infinity read as zero

inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) { p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }
}
inline void put64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) { p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }
}
inline std::uint32_t get32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

void iqWriteHeader(const IqHeader& h, std::uint8_t* out) noexcept {
    out[0] = 'F';
    out[1] = 'X';
    out[2] = 'I';
    out[3] = 'Q';
    put16(out + 4, 1);
    put16(out + 6, static_cast<std::uint16_t>(h.format));
    put32(out + 8, h.sampleRateHz);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &h.centreHz, 8);
    put64(out + 12, bits);
    put32(out + 20, h.sequence);
    put32(out + 24, 0);
}

bool iqReadHeader(const std::uint8_t* in, std::size_t n, IqHeader& h) noexcept {
    if (n < kIqHeaderBytes) { return false; }
    if (std::memcmp(in, "FXIQ", 4) != 0) { return false; }
    const unsigned version = in[4] | (in[5] << 8);
    const unsigned fmt = in[6] | (in[7] << 8);
    if (version != 1 || fmt < 1 || fmt > 3) { return false; }
    if (get32(in + 24) != 0) { return false; }
    h.format = static_cast<IqFormat>(fmt);
    h.sampleRateHz = get32(in + 8);
    std::uint64_t bits = 0;
    for (int i = 7; i >= 0; --i) { bits = (bits << 8) | in[12 + i]; }
    std::memcpy(&h.centreHz, &bits, 8);
    h.sequence = get32(in + 20);
    return true;
}

std::size_t iqBytesPerSample(IqFormat f) noexcept {
    switch (f) {
        case IqFormat::U8: return 2;
        case IqFormat::S16: return 4;
        case IqFormat::F32: return 8;
    }
    return 0;
}

void iqConvert(IqFormat f, const float* iq, std::size_t nComplex, std::uint8_t* out) noexcept {
    const std::size_t n = nComplex * 2;
    switch (f) {
        case IqFormat::U8:
            for (std::size_t i = 0; i < n; ++i) {
                const float x = clean(iq[i]);
                float v = x * 127.5f + 127.5f;
                v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
                out[i] = static_cast<std::uint8_t>(v + 0.5f);
            }
            break;
        case IqFormat::S16:
            for (std::size_t i = 0; i < n; ++i) {
                const float x = clean(iq[i]);
                float v = x * 32767.0f;
                v = v < -32767.0f ? -32767.0f : (v > 32767.0f ? 32767.0f : v);
                const int s = static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f);
                put16(out + 2 * i, static_cast<std::uint16_t>(static_cast<std::int16_t>(s)));
            }
            break;
        case IqFormat::F32: {
            static const bool le = hostIsLittleEndian();
            if (le) {
                for (std::size_t i = 0; i < n; ++i) {
                    const float x = clean(iq[i]);
                    std::memcpy(out + 4 * i, &x, 4);
                }
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    const float x = clean(iq[i]);
                    std::uint32_t b = 0;
                    std::memcpy(&b, &x, 4);
                    put32(out + 4 * i, b);
                }
            }
            break;
        }
    }
}

IqOutConfig IqOut::fix(const IqOutConfig& c) {
    IqOutConfig o = c;
    const std::size_t bps = iqBytesPerSample(o.format);
    if (o.chunkSamples == 0 && bps != 0) {
        if (o.udp) {
            const std::size_t room = o.udpCfg.maxDatagram - (o.header ? kIqHeaderBytes : 0);
            o.chunkSamples = std::max<std::size_t>(1, room / bps);
        } else {
            o.chunkSamples = kIqDefaultTcpChunkSamples;
        }
    }
    if (!o.udp && o.header && bps != 0) {
        IqHeader h;
        h.format = o.format;
        h.sampleRateHz = o.sampleRateHz;
        h.centreHz = o.centreHz;
        o.tcp.greeting.assign(kIqHeaderBytes, 0);
        iqWriteHeader(h, o.tcp.greeting.data());
    } else {
        o.tcp.greeting.clear();
    }
    return o;
}

IqOut::IqOut(const IqOutConfig& cfg) : cfg_(fix(cfg)), tcp_(cfg_.tcp), udp_(cfg_.udpCfg) {
    chunk_ = cfg_.chunkSamples;
    bps_ = iqBytesPerSample(cfg_.format);
    scratch_.assign(kIqHeaderBytes + chunk_ * bps_, 0);
}

bool IqOut::start(std::string& error) {
    if (bps_ == 0) {
        error = "unknown sample format";
        return false;
    }
    if (cfg_.header && (cfg_.sampleRateHz == 0 || !std::isfinite(cfg_.centreHz))) {
        error = "the header needs a sample rate and a finite centre frequency";
        return false;
    }
    if (chunk_ == 0) {
        error = "chunk size must be at least one sample";
        return false;
    }
    const std::size_t msg = chunk_ * bps_ + ((cfg_.udp && cfg_.header) ? kIqHeaderBytes : 0);
    if (cfg_.udp) {
        if (msg > cfg_.udpCfg.maxDatagram) {
            error = "a chunk does not fit in one datagram; use fewer samples per chunk or a larger datagram size";
            return false;
        }
        return udp_.start(error);
    }
    if (msg > tcp_.maxMessage()) {
        error = "a chunk does not fit the per-client queue or the ring; use fewer samples per chunk";
        return false;
    }
    return tcp_.start(error);
}

void IqOut::stop() {
    tcp_.stop();
    udp_.stop();
}

std::size_t IqOut::submit(const float* iq, std::size_t nComplex) noexcept {
    if (iq == nullptr || nComplex == 0) { return 0; }
    in_.fetch_add(nComplex, std::memory_order_relaxed);
    std::size_t queued = 0;
    const bool udpHeader = cfg_.udp && cfg_.header;
    const std::size_t off = udpHeader ? kIqHeaderBytes : 0;
    for (std::size_t done = 0; done < nComplex;) {
        const std::size_t n = std::min(chunk_, nComplex - done);
        iqConvert(cfg_.format, iq + 2 * done, n, scratch_.data() + off);
        bool ok;
        if (cfg_.udp) {
            if (udpHeader) {
                IqHeader h;
                h.format = cfg_.format;
                h.sampleRateHz = cfg_.sampleRateHz;
                h.centreHz = cfg_.centreHz;
                h.sequence = seq_++;
                iqWriteHeader(h, scratch_.data());
            }
            ok = udp_.send(scratch_.data(), off + n * bps_);
        } else {
            ok = tcp_.publish(scratch_.data(), n * bps_);
        }
        if (ok) {
            queued += n;
        } else {
            dropped_.fetch_add(n, std::memory_order_relaxed);
            chunksDropped_.fetch_add(1, std::memory_order_relaxed);
        }
        done += n;
    }
    queued_.fetch_add(queued, std::memory_order_relaxed);
    return queued;
}

IqStats IqOut::stats() const {
    IqStats s;
    s.samplesIn = in_.load();
    s.samplesQueued = queued_.load();
    s.samplesDropped = dropped_.load();
    s.chunksDropped = chunksDropped_.load();
    s.tcp = tcp_.stats();
    s.udp = udp_.stats();
    return s;
}

}  // namespace cascade::net
