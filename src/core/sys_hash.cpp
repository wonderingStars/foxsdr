// sys_hash.cpp - see sys_hash.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_hash.hpp"

#include <cstring>

namespace cascade::core::sys {
namespace {
#include "core/sys_sha_consts.inc"

inline std::uint32_t rotr32(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline std::uint64_t rotr64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
}  // namespace

Sha256::Sha256() { std::memcpy(h_, kSha256H0, sizeof h_); std::memset(buf_, 0, sizeof buf_); }

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        const std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += n;
    while (n > 0) {
        const std::size_t take = (64 - fill_ < n) ? 64 - fill_ : n;
        std::memcpy(buf_ + fill_, p, take);
        fill_ += take; p += take; n -= take;
        if (fill_ == 64) { block(buf_); fill_ = 0; }
    }
}

Digest256 Sha256::finish() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    const std::uint8_t zero = 0;
    while (fill_ != 56) { update(&zero, 1); }
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) { len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i)); }
    update(len, 8);
    Digest256 out{};
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24); out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8); out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return out;
}

Sha512::Sha512() { std::memcpy(h_, kSha512H0, sizeof h_); std::memset(buf_, 0, sizeof buf_); }

void Sha512::block(const std::uint8_t* p) {
    std::uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        std::uint64_t v = 0;
        for (int k = 0; k < 8; ++k) { v = (v << 8) | p[8 * i + k]; }
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        const std::uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const std::uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 80; ++i) {
        const std::uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        const std::uint64_t ch = (e & f) ^ (~e & g);
        const std::uint64_t t1 = h + S1 + ch + kSha512K[i] + w[i];
        const std::uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        const std::uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha512::update(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += n;
    while (n > 0) {
        const std::size_t take = (128 - fill_ < n) ? 128 - fill_ : n;
        std::memcpy(buf_ + fill_, p, take);
        fill_ += take; p += take; n -= take;
        if (fill_ == 128) { block(buf_); fill_ = 0; }
    }
}

Digest512 Sha512::finish() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    const std::uint8_t zero = 0;
    while (fill_ != 112) { update(&zero, 1); }
    std::uint8_t len[16] = {0};
    for (int i = 0; i < 8; ++i) { len[8 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i)); }
    update(len, 16);
    Digest512 out{};
    for (int i = 0; i < 8; ++i) { for (int k = 0; k < 8; ++k) { out[8 * i + k] = static_cast<std::uint8_t>(h_[i] >> (56 - 8 * k)); } }
    return out;
}

Digest256 sha256(const void* data, std::size_t n) { Sha256 h; h.update(data, n); return h.finish(); }
Digest512 sha512(const void* data, std::size_t n) { Sha512 h; h.update(data, n); return h.finish(); }

std::string toHex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

bool fromHex(const std::string& hex, std::string& out) {
    out.clear();
    if (hex.size() % 2 != 0) { return false; }
    auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string r;
    r.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int a = v(hex[i]), b = v(hex[i + 1]);
        if (a < 0 || b < 0) { return false; }
        r.push_back(static_cast<char>(a * 16 + b));
    }
    out = std::move(r);
    return true;
}

std::string sha256Hex(const std::string& bytes) {
    const auto d = sha256(bytes.data(), bytes.size());
    return toHex(d.data(), d.size());
}

}  // namespace cascade::core::sys
