// feedgen.hpp - synthetic I/Q feed generators for the plugin feed table (lane feedplan; docs/feature-records/feedplan.md).
//
// WHAT THIS IS AND IS NOT. Each builder below writes the signal a plugin is DESIGNED to decode, from the standard's tables, into the I/Q WAV form FoxSDR opens (2-channel PCM16 RIFF;
// src/source/iq_file_source.cpp). THE GENERATORS AND THE DECODERS SHARE AN AUTHOR (the plugin lanes' own test generators, written from the same reading of each standard, were the
// starting point and were rewritten here for a 48 kS/s I/Q file): a pass proves the two halves agree and that the plugin runs end to end in the engine; it does not prove either
// half right about the real world. Where a real recording exists the feed table puts it first.
//
// Every fixture carries what a real one carries: a noise floor, a carrier-frequency offset and a sample-rate error (Opt below). Everything is deterministic (fixed seeds), so the
// sha256 list in tests/feed/SHA256SUMS holds for any build. No real callsign, device identifier, name or address appears in any generated message: the texts are FOXSDR FIXTURE
// words and made-up numbers, so a screenshot of a generated feed is safe to publish.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace feedgen {

constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

// ---- random numbers ----------------------------------------------------------------------------------------------------------------------------
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed = 1) : s(seed ? seed * 0x9E3779B97F4A7C15ull : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uniform() { return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    double gauss() { return std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * kPi * uniform()); }
};

// ---- the I/Q container ------------------------------------------------------------------------------------------------------------------------
struct Iq {
    double rate = 48000.0;
    std::vector<float> i, q;
    std::size_t size() const { return i.size(); }
};

using Audio = std::vector<double>;   // real baseband audio at the I/Q rate

struct Opt {
    double snrDb = 40.0;      // signal power over noise power in the whole I/Q bandwidth (very clean, like a strong local signal)
    double cfoHz = 0.0;       // extra carrier-frequency offset on top of the carrier offset the builder is given
    double ppm = 0.0;         // sample-clock error: the whole signal is stretched by (1 + ppm * 1e-6)
    std::uint64_t seed = 1;
};

// ---- sample-rate error: a linear-interpolation resample of a real signal by `ratio` (the read position runs `ratio` times as fast) ------------------
inline Audio stretch(const Audio& a, double ppm) {
    if (ppm == 0.0 || a.size() < 2) { return a; }
    const double ratio = 1.0 + ppm * 1e-6;
    const std::size_t n = static_cast<std::size_t>(static_cast<double>(a.size() - 1) / ratio);
    Audio out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double p = static_cast<double>(k) * ratio;
        const std::size_t i0 = static_cast<std::size_t>(p);
        const double f = p - static_cast<double>(i0);
        out[k] = a[i0] * (1.0 - f) + a[std::min(i0 + 1, a.size() - 1)] * f;
    }
    return out;
}

// ---- the analytic signal: a real audio signal to I + jH(audio), by a 255-tap Hamming-windowed Hilbert FIR (group delay 127 samples, removed) --------
inline std::vector<cd> analytic(const Audio& a) {
    constexpr int kTaps = 255, kHalf = kTaps / 2;
    static std::vector<double> h;
    if (h.empty()) {
        h.assign(kTaps, 0.0);
        for (int n = -kHalf; n <= kHalf; ++n) {
            if (n % 2 != 0) { h[static_cast<std::size_t>(n + kHalf)] = (2.0 / (kPi * n)) * (0.54 + 0.46 * std::cos(kPi * n / (kHalf + 1))); }
        }
    }
    std::vector<cd> out(a.size());
    for (std::size_t k = 0; k < a.size(); ++k) {
        double acc = 0.0;
        for (int t = -kHalf; t <= kHalf; ++t) {
            if (t % 2 == 0) { continue; }
            const long idx = static_cast<long>(k) - t;
            if (idx < 0 || idx >= static_cast<long>(a.size())) { continue; }
            acc += h[static_cast<std::size_t>(t + kHalf)] * a[static_cast<std::size_t>(idx)];
        }
        out[k] = cd(a[k], acc);
    }
    return out;
}

// ---- modulators: audio at the I/Q rate onto a carrier at `offHz` from the file centre ------------------------------------------------------------
inline Iq finishIq(const std::vector<cd>& base, double rate, double offHz, const Opt& o) {
    Iq r;
    r.rate = rate;
    r.i.resize(base.size());
    r.q.resize(base.size());
    double ph = 0.0;
    const double dph = 2.0 * kPi * (offHz + o.cfoHz) / rate;
    double power = 0.0;
    for (std::size_t k = 0; k < base.size(); ++k) {
        const cd v = base[k] * cd(std::cos(ph), std::sin(ph));
        ph += dph;
        if (ph > 2.0 * kPi) { ph -= 2.0 * kPi; }
        r.i[k] = static_cast<float>(v.real());
        r.q[k] = static_cast<float>(v.imag());
        power += std::norm(v);
    }
    if (o.snrDb < 150.0 && !base.empty()) {
        // noise power is set against the power of the ACTIVE part of the signal (samples above a tenth of the mean), as a receiver's SNR is
        const double mean = power / static_cast<double>(base.size());
        double act = 0.0;
        std::size_t na = 0;
        for (std::size_t k = 0; k < base.size(); ++k) {
            const double p = std::norm(base[k]);
            if (p > 0.1 * mean) { act += p; ++na; }
        }
        const double sp = na ? act / static_cast<double>(na) : mean;
        const double sigma = std::sqrt(sp / std::pow(10.0, o.snrDb / 10.0) / 2.0);
        Rng g(o.seed * 7919u + 13u);
        for (std::size_t k = 0; k < base.size(); ++k) {
            r.i[k] += static_cast<float>(sigma * g.gauss());
            r.q[k] += static_cast<float>(sigma * g.gauss());
        }
    }
    return r;
}

// Upper sideband: the audio's spectrum placed ABOVE the carrier offset (a USB receiver tuned at the carrier offset hears the audio).
inline Iq usb(const Audio& audio, double rate, double offHz, const Opt& o = Opt()) {
    return finishIq(analytic(stretch(audio, o.ppm)), rate, offHz, o);
}
// Amplitude modulation: (1 + depth * audio) on the carrier; audio is normalised to +-1.
inline Iq am(const Audio& audio, double rate, double offHz, double depth, const Opt& o = Opt()) {
    const Audio a = stretch(audio, o.ppm);
    std::vector<cd> b(a.size());
    for (std::size_t k = 0; k < a.size(); ++k) { b[k] = cd(0.5 * (1.0 + depth * a[k]), 0.0); }
    return finishIq(b, rate, offHz, o);
}
// Narrow FM: instantaneous frequency = deviation * audio (audio normalised to +-1).
inline Iq nfm(const Audio& audio, double rate, double offHz, double devHz, const Opt& o = Opt()) {
    const Audio a = stretch(audio, o.ppm);
    std::vector<cd> b(a.size());
    double ph = 0.0;
    for (std::size_t k = 0; k < a.size(); ++k) {
        ph += 2.0 * kPi * devHz * a[k] / rate;
        b[k] = cd(0.5 * std::cos(ph), 0.5 * std::sin(ph));
    }
    return finishIq(b, rate, offHz, o);
}
// A complex baseband used as it stands (the builder made it already).
inline Iq complexBase(const std::vector<cd>& base, double rate, double offHz, const Opt& o = Opt()) { return finishIq(base, rate, offHz, o); }

// ---- the WAV writer: 2-channel PCM16, scaled so the peak is `peak` of full scale (the file carries no calibration) ---------------------------------
inline void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
inline void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    put16(v, static_cast<std::uint16_t>(x & 0xFFFF));
    put16(v, static_cast<std::uint16_t>(x >> 16));
}
inline std::vector<std::uint8_t> wavBytes(const Iq& iq, double peak = 0.7) {
    float m = 1e-9f;
    for (std::size_t k = 0; k < iq.size(); ++k) { m = std::max({m, std::fabs(iq.i[k]), std::fabs(iq.q[k])}); }
    const double g = peak * 32767.0 / m;
    std::vector<std::uint8_t> v;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(iq.size() * 4);
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    put32(v, 36 + dataBytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(v, 16);
    put16(v, 1);
    put16(v, 2);
    put32(v, static_cast<std::uint32_t>(iq.rate + 0.5));
    put32(v, static_cast<std::uint32_t>(iq.rate + 0.5) * 4);
    put16(v, 4);
    put16(v, 16);
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    put32(v, dataBytes);
    v.reserve(v.size() + dataBytes);
    for (std::size_t k = 0; k < iq.size(); ++k) {
        put16(v, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(iq.i[k] * g))));
        put16(v, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(iq.q[k] * g))));
    }
    return v;
}
inline bool writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { return false; }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

// ---- SHA-256 (FIPS 180-4), for the checksum list -------------------------------------------------------------------------------------------------
inline std::string sha256(const std::vector<std::uint8_t>& data) {
    static const std::uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> m = data;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8u;
    m.push_back(0x80);
    while (m.size() % 64 != 56) { m.push_back(0); }
    for (int s = 56; s >= 0; s -= 8) { m.push_back(static_cast<std::uint8_t>(bits >> s)); }
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[64];
        for (int t = 0; t < 16; ++t) {
            w[t] = (static_cast<std::uint32_t>(m[off + 4 * t]) << 24) | (static_cast<std::uint32_t>(m[off + 4 * t + 1]) << 16) | (static_cast<std::uint32_t>(m[off + 4 * t + 2]) << 8) |
                   static_cast<std::uint32_t>(m[off + 4 * t + 3]);
        }
        for (int t = 16; t < 64; ++t) {
            const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 64; ++t) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + K[t] + w[t];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int k = 0; k < 8; ++k) { std::snprintf(out + 8 * k, 9, "%08x", h[k]); }
    return std::string(out);
}

// ---- audio helpers ------------------------------------------------------------------------------------------------------------------------------
inline void silence(Audio& a, double seconds, double rate) { a.insert(a.end(), static_cast<std::size_t>(seconds * rate + 0.5), 0.0); }

// A tone burst with raised-cosine edges (`rise` seconds), appended; phase continuous within the burst.
inline void tone(Audio& a, double hz, double seconds, double rate, double amp = 1.0, double rise = 0.002) {
    const std::size_t n = static_cast<std::size_t>(seconds * rate + 0.5);
    const std::size_t r = static_cast<std::size_t>(rise * rate);
    for (std::size_t k = 0; k < n; ++k) {
        double e = 1.0;
        if (k < r) { e = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(k) / static_cast<double>(r)); }
        if (n - 1 - k < r) { e = std::min(e, 0.5 - 0.5 * std::cos(kPi * static_cast<double>(n - 1 - k) / static_cast<double>(r))); }
        a.push_back(amp * e * std::sin(2.0 * kPi * hz * static_cast<double>(k) / rate));
    }
}
inline void addTone(Audio& a, double hz, double amp, double rate, double startS = 0.0) {
    for (std::size_t k = static_cast<std::size_t>(startS * rate); k < a.size(); ++k) { a[k] += amp * std::sin(2.0 * kPi * hz * static_cast<double>(k) / rate); }
}
inline void normalise(Audio& a, double peak = 0.9) {
    double m = 1e-12;
    for (double x : a) { m = std::max(m, std::fabs(x)); }
    for (double& x : a) { x *= peak / m; }
}

}  // namespace feedgen
