// sys_ed25519.cpp - see sys_ed25519.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_ed25519.hpp"

#include <cstring>
#include <vector>

#include "core/sys_hash.hpp"

namespace cascade::core::sys {
namespace {

using i64 = std::int64_t;
using gf = i64[16];

const gf kGf0 = {0};
const gf kGf1 = {1};
// Computed from the definitions (see sys_ed25519.hpp), little-endian 16-bit limbs of the value mod p = 2^255 - 19.
const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// The group order L = 2^252 + 27742317777372353535851937790883648493, little-endian bytes.
const i64 kL[32] = {237, 211, 245, 92, 26, 99, 18, 88, 214, 156, 247, 162, 222, 249, 222, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16};

void set25519(gf r, const gf a) { for (int i = 0; i < 16; ++i) { r[i] = a[i]; } }

void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (i64(1) << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void sel25519(gf p, gf q, int b) {
    const i64 c = ~(i64(b) - 1);
    for (int i = 0; i < 16; ++i) { const i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

void pack25519(std::uint8_t* o, const gf n) {
    gf m, t;
    set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) { o[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff); o[2 * i + 1] = static_cast<std::uint8_t>(t[i] >> 8); }
}

bool neq25519(const gf a, const gf b) {
    std::uint8_t c[32], d[32];
    pack25519(c, a); pack25519(d, b);
    return std::memcmp(c, d, 32) != 0;
}

int par25519(const gf a) { std::uint8_t d[32]; pack25519(d, a); return d[0] & 1; }

void unpack25519(gf o, const std::uint8_t* n) {
    for (int i = 0; i < 16; ++i) { o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8); }
    o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) { o[i] = a[i] + b[i]; } }
void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) { o[i] = a[i] - b[i]; } }

void M(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; ++i) { t[i] = 0; }
    for (int i = 0; i < 16; ++i) { for (int j = 0; j < 16; ++j) { t[i + j] += a[i] * b[j]; } }
    for (int i = 0; i < 15; ++i) { t[i] += 38 * t[i + 16]; }
    for (int i = 0; i < 16; ++i) { o[i] = t[i]; }
    car25519(o); car25519(o);
}

void S(gf o, const gf a) { M(o, a, a); }

void inv25519(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; --a) { S(c, c); if (a != 2 && a != 4) { M(c, c, i); } }
    set25519(o, c);
}

void pow2523(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 250; a >= 0; --a) { S(c, c); if (a != 1) { M(c, c, i); } }
    set25519(o, c);
}

// Extended coordinates (X, Y, Z, T), RFC 8032 5.1.4.
void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, kD2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}

void cswap(gf p[4], gf q[4], std::uint8_t b) { for (int i = 0; i < 4; ++i) { sel25519(p[i], q[i], b); } }

void pack(std::uint8_t* r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= static_cast<std::uint8_t>(par25519(tx) << 7);
}

void scalarmult(gf p[4], gf q[4], const std::uint8_t* s) {
    set25519(p[0], kGf0); set25519(p[1], kGf1); set25519(p[2], kGf1); set25519(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        const std::uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

void scalarbase(gf p[4], const std::uint8_t* s) {
    gf q[4];
    set25519(q[0], kX); set25519(q[1], kY); set25519(q[2], kGf1); M(q[3], kX, kY);
    scalarmult(p, q, s);
}

// r = x mod L, x being 64 signed limbs of 8 bits.
void modL(std::uint8_t* r, i64 x[64]) {
    i64 carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * kL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) { x[j] -= carry * kL[j]; }
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<std::uint8_t>(x[i] & 255);
    }
}

void reduce(std::uint8_t r[64]) {
    i64 x[64];
    for (int i = 0; i < 64; ++i) { x[i] = static_cast<i64>(r[i]); }
    for (int i = 0; i < 64; ++i) { r[i] = 0; }
    modL(r, x);
}

// true when the 32-byte little-endian value (top bit cleared) is below p = 2^255 - 19.
bool yBelowP(const std::uint8_t* s) {
    if ((s[31] & 0x7f) != 0x7f) { return true; }
    for (int i = 30; i >= 1; --i) { if (s[i] != 0xff) { return true; } }
    return s[0] < 0xed;
}

// r = -(decoded point) or false (RFC 8032 5.1.3 decoding, then negated for the verification sum).
bool unpackneg(gf r[4], const std::uint8_t* p) {
    if (!yBelowP(p)) { return false; }
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], kGf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, kD);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den); S(den4, den2); M(den6, den4, den2);
    M(t, den6, num); M(t, t, den);
    pow2523(t, t);
    M(t, t, num); M(t, t, den); M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) { M(r[0], r[0], kI); }
    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) { return false; }
    const int sign = p[31] >> 7;
    if (!neq25519(r[0], kGf0) && sign == 1) { return false; }   // x = 0 may not carry the sign bit
    if (par25519(r[0]) == sign) { Z(r[0], kGf0, r[0]); }
    M(r[3], r[0], r[1]);
    return true;
}

bool sLessThanL(const std::uint8_t* s) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < kL[i]) { return true; }
        if (s[i] > kL[i]) { return false; }
    }
    return false;   // equal
}

Digest512 expandSeed(const Ed25519Seed& seed) {
    Digest512 d = sha512(seed.data(), seed.size());
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    return d;
}

}  // namespace

Ed25519PublicKey ed25519PublicKey(const Ed25519Seed& seed) {
    const Digest512 d = expandSeed(seed);
    gf p[4];
    scalarbase(p, d.data());
    Ed25519PublicKey pk{};
    pack(pk.data(), p);
    return pk;
}

Ed25519Signature ed25519Sign(const Ed25519Seed& seed, const std::uint8_t* msg, std::size_t n) {
    const Digest512 d = expandSeed(seed);
    const Ed25519PublicKey pk = ed25519PublicKey(seed);

    Sha512 h1;
    h1.update(d.data() + 32, 32);
    h1.update(msg, n);
    Digest512 rr = h1.finish();
    reduce(rr.data());
    gf p[4];
    scalarbase(p, rr.data());
    Ed25519Signature sig{};
    pack(sig.data(), p);   // R

    Sha512 h2;
    h2.update(sig.data(), 32);
    h2.update(pk.data(), 32);
    h2.update(msg, n);
    Digest512 hh = h2.finish();
    reduce(hh.data());

    i64 x[64];
    for (int i = 0; i < 64; ++i) { x[i] = 0; }
    for (int i = 0; i < 32; ++i) { x[i] = static_cast<i64>(rr[i]); }
    for (int i = 0; i < 32; ++i) { for (int j = 0; j < 32; ++j) { x[i + j] += static_cast<i64>(hh[i]) * static_cast<i64>(d[j]); } }
    modL(sig.data() + 32, x);
    return sig;
}

bool ed25519Verify(const Ed25519PublicKey& pub, const std::uint8_t* msg, std::size_t n, const Ed25519Signature& sig) {
    if (!sLessThanL(sig.data() + 32)) { return false; }
    gf q[4];
    if (!unpackneg(q, pub.data())) { return false; }

    Sha512 h;
    h.update(sig.data(), 32);
    h.update(pub.data(), 32);
    h.update(msg, n);
    Digest512 k = h.finish();
    reduce(k.data());

    gf p[4];
    scalarmult(p, q, k.data());          // [k](-A)
    gf b[4];
    scalarbase(b, sig.data() + 32);      // [S]B
    add(p, b);
    std::uint8_t t[32];
    pack(t, p);
    return std::memcmp(t, sig.data(), 32) == 0;
}

}  // namespace cascade::core::sys
