// Tests for the FLAC writer (core/flac_writer.hpp).
//
// THE DECODER BELOW IS WRITTEN HERE FROM RFC 9639 (https://www.rfc-editor.org/rfc/rfc9639.html, read 2026-11-11), NOT FROM THE WRITER: it has its own bit
// reader, its own bitwise CRC-8 / CRC-16, its own frame-header parser, and its own MD5 check (RFC 1321 vectors, and on Windows the system's CNG MD5 as
// a third opinion). It decodes constant, verbatim and fixed-predictor subframes, the four channel assignments, wasted bits and both Rice methods.
// Where a FLAC-reading program is on the PC (ffmpeg, already installed; nothing was installed for this) the files are also decoded by it with CRC and
// MD5 checking switched on, and the PCM compared byte for byte.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "alloc_probe.hpp"
#include "core/flac_writer.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {
namespace fs = std::filesystem;
namespace fl = cascade::core::flac;
using Bytes = std::vector<std::uint8_t>;

fs::path g_dir;

Bytes readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- independent pieces
std::uint8_t refCrc8(const Bytes& b, std::size_t n) {   // x^8 + x^2 + x + 1, init 0, MSB first
    unsigned crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const unsigned in = (b[i] >> bit) & 1u;
            const unsigned top = (crc >> 7) & 1u;
            crc = (crc << 1) & 0xFF;
            if ((in ^ top) != 0) { crc ^= 0x07; }
        }
    }
    return static_cast<std::uint8_t>(crc);
}
std::uint16_t refCrc16(const Bytes& b, std::size_t from, std::size_t to) {   // x^16 + x^15 + x^2 + 1, init 0
    unsigned crc = 0;
    for (std::size_t i = from; i < to; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const unsigned in = (b[i] >> bit) & 1u;
            const unsigned top = (crc >> 15) & 1u;
            crc = (crc << 1) & 0xFFFF;
            if ((in ^ top) != 0) { crc ^= 0x8005; }
        }
    }
    return static_cast<std::uint16_t>(crc);
}

std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
std::string md5Hex(const std::string& s) {
    fl::Md5 m;
    m.update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    const auto d = m.finish();
    return hex(d.data(), 16);
}

#if defined(_WIN32)
// The Windows CNG MD5, a second implementation to compare the writer's MD5 with.
bool cngMd5(const Bytes& data, std::uint8_t out[16]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) { return false; }
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok) { ok = BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0; }
    if (ok) { ok = BCryptFinishHash(h, out, 16, 0) == 0; }
    if (h != nullptr) { BCryptDestroyHash(h); }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
#endif

struct BitReader {
    const Bytes& b;
    std::size_t pos = 0;   // bit position
    bool bad = false;
    explicit BitReader(const Bytes& bytes, std::size_t byteStart) : b(bytes), pos(byteStart * 8) {}
    unsigned bit() {
        if (pos >= b.size() * 8) { bad = true; return 0; }
        const unsigned v = (b[pos >> 3] >> (7 - (pos & 7))) & 1u;
        ++pos;
        return v;
    }
    std::uint64_t u(int n) { std::uint64_t v = 0; for (int i = 0; i < n; ++i) { v = (v << 1) | bit(); } return v; }
    std::int64_t s(int n) {
        if (n == 0) { return 0; }
        std::int64_t v = static_cast<std::int64_t>(u(n));
        if ((v >> (n - 1)) & 1) { v -= (1ll << n); }
        return v;
    }
    std::uint64_t unary() { std::uint64_t q = 0; while (bit() == 0 && !bad) { ++q; } return q; }
    void align() { pos = (pos + 7) & ~std::size_t{7}; }
};

struct DecInfo {
    bool ok = false;
    std::string why;
    unsigned sampleRate = 0, channels = 0, bps = 0;
    std::uint64_t total = 0;
    unsigned minBlock = 0, maxBlock = 0, minFrame = 0, maxFrame = 0;
    std::uint8_t md5[16] = {};
    bool md5Zero = true;
    bool hasSeek = false;
    struct Seek { std::uint64_t sample, offset; unsigned n; };
    std::vector<Seek> seek;
    std::size_t firstFrameAt = 0;
    std::vector<std::int32_t> pcm;               // interleaved
    int framesDecoded = 0;
    std::vector<int> assignments;                // per frame
    std::vector<std::size_t> frameStarts;        // byte offsets
    int constantSub = 0, verbatimSub = 0, fixedSub = 0, wastedSub = 0, wideRice = 0;
    std::vector<int> fixedOrders;
};

DecInfo decode(const Bytes& b) {
    DecInfo d;
    auto fail = [&](const std::string& w) { d.why = w; return d; };
    if (b.size() < 8 || b[0] != 'f' || b[1] != 'L' || b[2] != 'a' || b[3] != 'C') { return fail("no fLaC"); }
    std::size_t p = 4;
    bool last = false, sawInfo = false;
    while (!last) {
        if (p + 4 > b.size()) { return fail("metadata cut"); }
        last = (b[p] & 0x80) != 0;
        const int type = b[p] & 0x7F;
        const std::size_t len = (static_cast<std::size_t>(b[p + 1]) << 16) | (static_cast<std::size_t>(b[p + 2]) << 8) | b[p + 3];
        p += 4;
        if (p + len > b.size()) { return fail("metadata body cut"); }
        if (type == 127) { return fail("forbidden block type"); }
        if (!sawInfo) {
            if (type != 0 || len != 34) { return fail("STREAMINFO not first"); }
            sawInfo = true;
            BitReader r(b, p);
            d.minBlock = static_cast<unsigned>(r.u(16)); d.maxBlock = static_cast<unsigned>(r.u(16));
            d.minFrame = static_cast<unsigned>(r.u(24)); d.maxFrame = static_cast<unsigned>(r.u(24));
            d.sampleRate = static_cast<unsigned>(r.u(20)); d.channels = static_cast<unsigned>(r.u(3)) + 1; d.bps = static_cast<unsigned>(r.u(5)) + 1;
            d.total = r.u(36);
            for (int i = 0; i < 16; ++i) { d.md5[i] = static_cast<std::uint8_t>(r.u(8)); if (d.md5[i] != 0) { d.md5Zero = false; } }
        } else if (type == 3) {
            d.hasSeek = true;
            if (len % 18 != 0) { return fail("seek table length"); }
            BitReader r(b, p);
            for (std::size_t i = 0; i < len / 18; ++i) {
                DecInfo::Seek sp;
                sp.sample = r.u(64); sp.offset = r.u(64); sp.n = static_cast<unsigned>(r.u(16));
                d.seek.push_back(sp);
            }
        }
        p += len;
    }
    if (!sawInfo) { return fail("no STREAMINFO"); }
    d.firstFrameAt = p;

    std::uint64_t expectedSample = 0;
    while (p < b.size()) {
        const std::size_t frameStart = p;
        d.frameStarts.push_back(frameStart);
        if (p + 6 > b.size()) { return fail("frame cut"); }
        if (b[p] != 0xFF || (b[p + 1] & 0xFE) != 0xF8) { return fail("bad sync at " + std::to_string(p)); }
        const bool variable = (b[p + 1] & 1) != 0;
        if (variable) { return fail("variable-size stream not expected"); }
        const int bsCode = b[p + 2] >> 4, srCode = b[p + 2] & 15;
        const int chCode = b[p + 3] >> 4, bpsCode = (b[p + 3] >> 1) & 7;
        if ((b[p + 3] & 1) != 0) { return fail("reserved bit set"); }
        std::size_t q = p + 4;
        // coded number
        std::uint64_t frameNo = 0;
        {
            const unsigned lead = b[q];
            int extra = 0;
            if ((lead & 0x80) == 0) { frameNo = lead; }
            else {
                while (((lead << extra) & 0x80) != 0 && extra < 8) { ++extra; }
                extra -= 1;   // number of continuation bytes
                frameNo = lead & ((1u << (6 - extra)) - 1);
            }
            ++q;
            for (int i = 0; i < extra; ++i) {
                if ((b[q] & 0xC0) != 0x80) { return fail("bad continuation"); }
                frameNo = (frameNo << 6) | (b[q] & 0x3F); ++q;
            }
        }
        int blocksize = 0;
        if (bsCode == 1) { blocksize = 192; }
        else if (bsCode >= 2 && bsCode <= 5) { blocksize = 576 << (bsCode - 2); }
        else if (bsCode == 6) { blocksize = b[q] + 1; ++q; }
        else if (bsCode == 7) { blocksize = ((b[q] << 8) | b[q + 1]) + 1; q += 2; }
        else if (bsCode >= 8) { blocksize = 256 << (bsCode - 8); }
        else { return fail("reserved block size"); }
        unsigned rate = d.sampleRate;
        static const unsigned rt[16] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0};
        if (srCode >= 1 && srCode <= 11) { rate = rt[srCode]; }
        else if (srCode == 12) { rate = b[q] * 1000u; ++q; }
        else if (srCode == 13) { rate = (b[q] << 8) | b[q + 1]; q += 2; }
        else if (srCode == 14) { rate = ((b[q] << 8) | b[q + 1]) * 10u; q += 2; }
        else if (srCode == 15) { return fail("forbidden rate code"); }
        if (rate != d.sampleRate) { return fail("frame rate differs from STREAMINFO"); }
        if (q >= b.size()) { return fail("header cut"); }
        // CRC-8 covers [p, q)
        {
            Bytes hdr(b.begin() + static_cast<std::ptrdiff_t>(p), b.begin() + static_cast<std::ptrdiff_t>(q));
            if (refCrc8(hdr, hdr.size()) != b[q]) { return fail("CRC-8 mismatch at frame " + std::to_string(d.framesDecoded)); }
        }
        ++q;
        if (frameNo != static_cast<std::uint64_t>(d.framesDecoded)) { return fail("frame number out of sequence"); }
        unsigned bps = d.bps;
        switch (bpsCode) { case 1: bps = 8; break; case 2: bps = 12; break; case 4: bps = 16; break; case 5: bps = 20; break; case 6: bps = 24; break; case 7: bps = 32; break; case 0: break; default: return fail("reserved bps"); }
        if (bps != d.bps) { return fail("frame bps differs"); }
        int nch = 0;
        if (chCode <= 7) { nch = chCode + 1; } else if (chCode <= 10) { nch = 2; } else { return fail("reserved channel assignment"); }
        if (static_cast<unsigned>(nch) != d.channels) { return fail("frame channels differ"); }
        d.assignments.push_back(chCode);

        BitReader br(b, q);
        std::vector<std::vector<std::int64_t>> ch(static_cast<std::size_t>(nch), std::vector<std::int64_t>(static_cast<std::size_t>(blocksize)));
        for (int c = 0; c < nch; ++c) {
            int sbps = static_cast<int>(bps);
            if ((chCode == 8 && c == 1) || (chCode == 9 && c == 0) || (chCode == 10 && c == 1)) { ++sbps; }
            if (br.bit() != 0) { return fail("subframe pad bit"); }
            const int type = static_cast<int>(br.u(6));
            int wasted = 0;
            if (br.bit() != 0) { wasted = 1; while (br.bit() == 0 && !br.bad) { ++wasted; } ++d.wastedSub; }
            const int eb = sbps - wasted;
            auto& x = ch[static_cast<std::size_t>(c)];
            if (type == 0) {
                const std::int64_t v = br.s(eb);
                for (auto& s : x) { s = v; }
                ++d.constantSub;
            } else if (type == 1) {
                for (auto& s : x) { s = br.s(eb); }
                ++d.verbatimSub;
            } else if (type >= 8 && type <= 12) {
                const int order = type - 8;
                ++d.fixedSub;
                d.fixedOrders.push_back(order);
                for (int i = 0; i < order; ++i) { x[static_cast<std::size_t>(i)] = br.s(eb); }
                const int method = static_cast<int>(br.u(2));
                if (method > 1) { return fail("reserved residual method"); }
                if (method == 1) { ++d.wideRice; }
                const int po = static_cast<int>(br.u(4));
                const int parts = 1 << po;
                if (blocksize % parts != 0 || (blocksize >> po) <= order) { return fail("bad partition order"); }
                const int psz = blocksize >> po;
                for (int part = 0; part < parts; ++part) {
                    const int k = static_cast<int>(br.u(method == 0 ? 4 : 5));
                    const int esc = method == 0 ? 15 : 31;
                    const int first = part == 0 ? order : part * psz;
                    const int end = (part + 1) * psz;
                    if (k == esc) { return fail("escape code not produced by the writer"); }
                    {
                        for (int i = first; i < end; ++i) {
                            const std::uint64_t qq = br.unary();
                            const std::uint64_t r = k > 0 ? br.u(k) : 0;
                            const std::uint64_t v = (qq << k) | r;
                            x[static_cast<std::size_t>(i)] = (v & 1) ? -static_cast<std::int64_t>((v + 1) >> 1) : static_cast<std::int64_t>(v >> 1);
                        }
                    }
                }
                // x[order..] now hold residuals; reconstruct in place
                for (int i = order; i < blocksize; ++i) {
                    auto A = [&](int back) { return x[static_cast<std::size_t>(i - back)]; };
                    std::int64_t pred = 0;
                    switch (order) {
                        case 0: pred = 0; break;
                        case 1: pred = A(1); break;
                        case 2: pred = 2 * A(1) - A(2); break;
                        case 3: pred = 3 * A(1) - 3 * A(2) + A(3); break;
                        case 4: pred = 4 * A(1) - 6 * A(2) + 4 * A(3) - A(4); break;
                    }
                    x[static_cast<std::size_t>(i)] += pred;
                }
            } else {
                return fail("subframe type " + std::to_string(type) + " not produced by the writer");
            }
            if (br.bad) { return fail("bit reader ran past the end"); }
            if (wasted > 0) { for (auto& s : x) { s *= (1ll << wasted); } }
        }
        br.align();
        q = br.pos / 8;
        if (q + 2 > b.size()) { return fail("footer cut"); }
        const std::uint16_t want = static_cast<std::uint16_t>((b[q] << 8) | b[q + 1]);
        if (refCrc16(b, p, q) != want) { return fail("CRC-16 mismatch at frame " + std::to_string(d.framesDecoded)); }
        q += 2;
        // stereo reconstruction
        for (int i = 0; i < blocksize; ++i) {
            const std::size_t k = static_cast<std::size_t>(i);
            if (nch == 1) { d.pcm.push_back(static_cast<std::int32_t>(ch[0][k])); continue; }
            std::int64_t L, R;
            if (chCode == 1) { L = ch[0][k]; R = ch[1][k]; }
            else if (chCode == 8) { L = ch[0][k]; R = L - ch[1][k]; }
            else if (chCode == 9) { R = ch[1][k]; L = ch[0][k] + R; }
            else {
                const std::int64_t S = ch[1][k];
                std::int64_t M = ch[0][k] * 2;
                if ((S & 1) != 0) { M += 1; }
                L = (M + S) >> 1; R = (M - S) >> 1;
            }
            d.pcm.push_back(static_cast<std::int32_t>(L)); d.pcm.push_back(static_cast<std::int32_t>(R));
        }
        expectedSample += static_cast<std::uint64_t>(blocksize);
        ++d.framesDecoded;
        p = q;
    }
    d.ok = true;
    return d;
}

// MD5 of decoded PCM the way the RFC defines it (interleaved little-endian, sign-extended to whole bytes), over the decoder's own samples.
Bytes pcmBytes(const std::vector<std::int32_t>& pcm, int bps) {
    Bytes out;
    for (std::int32_t v : pcm) { for (int b = 0; b < bps / 8; ++b) { out.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * b))); } }
    return out;
}
std::string md5OfBytes(const Bytes& b) {
    fl::Md5 m;
    m.update(b.data(), b.size());
    const auto d = m.finish();
    return hex(d.data(), 16);
}

// ---------------------------------------------------------------- the encode helper
struct Enc { Bytes file; bool ok = false; std::string err; };
Enc encodeFile(const std::string& name, const fl::Options& o, const std::vector<std::int32_t>& inter, std::size_t chunk = 1000) {
    Enc e;
    const std::string path = (g_dir / name).string();
    fl::Writer w;
    if (!w.open(path, o, &e.err)) { return e; }
    const std::size_t ch = static_cast<std::size_t>(o.channels);
    const std::size_t frames = inter.size() / ch;
    for (std::size_t i = 0; i < frames; i += chunk) {
        if (!w.write(inter.data() + i * ch, std::min(chunk, frames - i), &e.err)) { return e; }
    }
    e.ok = w.finish(&e.err);
    e.file = readFile(path);
    return e;
}

std::int32_t clampTo(double v, int bps) {
    const double hi = std::ldexp(1.0, bps - 1) - 1, lo = -std::ldexp(1.0, bps - 1);
    return static_cast<std::int32_t>(std::llround(std::max(lo, std::min(hi, v))));
}

// signals (interleaved)
std::vector<std::int32_t> sigSine(std::size_t n, int ch, int bps, double fs, double amp) {
    std::vector<std::int32_t> v(n * static_cast<std::size_t>(ch));
    const double full = std::ldexp(1.0, bps - 1);
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < ch; ++c) {
            v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] =
                clampTo(full * amp * std::sin(2 * 3.14159265358979 * (440.0 + 70.0 * c) * static_cast<double>(i) / fs), bps);
        }
    }
    return v;
}
std::vector<std::int32_t> sigNoise(std::size_t n, int ch, int bps, double amp, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const double full = std::ldexp(1.0, bps - 1);
    std::vector<std::int32_t> v(n * static_cast<std::size_t>(ch));
    for (auto& s : v) { s = clampTo(std::round(full * amp * u(g)), bps); }
    return v;
}
std::vector<std::int32_t> sigSpeech(std::size_t n, int ch, int bps, double fs, std::uint32_t seed) {
    const auto f = sbgen::speechLike(n, fs, 0.5, seed);
    const double full = std::ldexp(1.0, bps - 1);
    std::vector<std::int32_t> v(n * static_cast<std::size_t>(ch));
    for (std::size_t i = 0; i < n; ++i) { for (int c = 0; c < ch; ++c) { v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = clampTo(f[i] * full, bps); } }
    return v;
}

int runCmd(const std::string& cmd) { return std::system(("cmd /c \"" + cmd + "\"").c_str()); }
bool haveFfmpeg() {
    static int have = -1;
    if (have < 0) { have = runCmd("ffmpeg -version > NUL 2>&1") == 0 ? 1 : 0; }
    return have == 1;
}

// Decodes with ffmpeg (CRC + MD5 checking on); returns the raw PCM bytes and the stderr text.
bool ffmpegDecode(const fs::path& flac, int bps, Bytes* pcm, std::string* errText) {
    const fs::path out = g_dir / "recm-ff.pcm", er = g_dir / "recm-ff.err";
    fs::remove(out); fs::remove(er);
    const char* fmt = bps == 16 ? "s16le" : "s24le";
    const char* codec = bps == 16 ? "pcm_s16le" : "pcm_s24le";
    const int rc = runCmd("ffmpeg -nostdin -v error -err_detect aggressive -i \"" + flac.string() + "\" -c:a " + codec + " -f " + fmt + " \"" + out.string() + "\" 2> \"" + er.string() + "\"");
    *pcm = readFile(out);
    const Bytes e = readFile(er);
    *errText = std::string(e.begin(), e.end());
    return rc == 0;
}

// ================================================================================================================================ tests
void testMd5() {
    // RFC 1321 appendix A.5 test suite
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");
    // chunking must not matter (55, 56, 63, 64, 65 byte boundaries)
    for (std::size_t n : {55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 1000u}) {
        std::string s(n, 'x');
        for (std::size_t i = 0; i < n; ++i) { s[i] = static_cast<char>('a' + i % 26); }
        fl::Md5 m;
        for (std::size_t i = 0; i < n; i += 7) { m.update(reinterpret_cast<const std::uint8_t*>(s.data()) + i, std::min<std::size_t>(7, n - i)); }
        const auto d = m.finish();
        CHECK(hex(d.data(), 16) == md5Hex(s));
#if defined(_WIN32)
        std::uint8_t ref[16];
        CHECK(cngMd5(Bytes(s.begin(), s.end()), ref));
        CHECK(hex(ref, 16) == hex(d.data(), 16));
#endif
    }
}

void testRoundTrips() {
    struct Case { int ch, bps; std::uint32_t rate; int block; std::size_t n; const char* sig; };
    const std::vector<Case> cases = {
        {1, 16, 48000, 4096, 0, "sine"},     {1, 16, 48000, 4096, 1, "sine"},      {2, 16, 48000, 4096, 1, "noise"},   {1, 16, 44100, 4096, 15, "noise"},
        {2, 24, 96000, 4096, 255, "sine"},   {1, 16, 48000, 4096, 4095, "speech"}, {1, 16, 48000, 4096, 4096, "speech"}, {1, 16, 48000, 4096, 4097, "speech"},
        {2, 16, 44100, 4096, 10000, "speech"}, {2, 24, 48000, 4096, 10000, "speech"}, {1, 24, 48000, 4096, 9000, "noise"}, {2, 16, 8000, 256, 5000, "sine"},
        {2, 16, 22050, 1152, 5000, "speech"}, {1, 16, 12345, 1000, 7000, "speech"}, {2, 16, 5000, 300, 3000, "noise"}, {1, 16, 700001, 32768, 70000, "sine"},
        {2, 24, 192000, 8192, 20000, "sine"}, {1, 16, 88200, 576, 4000, "speech"}, {2, 16, 176400, 2304, 9000, "sine"}, {1, 16, 32000, 4608, 9300, "speech"},
        {2, 16, 16000, 512, 2049, "noise"},  {1, 24, 24000, 2048, 5000, "speech"}, {2, 16, 48000, 16384, 33000, "speech"}, {2, 16, 48000, 1024, 3000, "sine"},
    };
    int idx = 0;
    for (const Case& c : cases) {
        std::vector<std::int32_t> in;
        if (std::string(c.sig) == "sine") { in = sigSine(c.n, c.ch, c.bps, c.rate, 0.6); }
        else if (std::string(c.sig) == "noise") { in = sigNoise(c.n, c.ch, c.bps, 1.0, 100 + static_cast<unsigned>(idx)); }
        else { in = sigSpeech(c.n, c.ch, c.bps, c.rate, 7 + static_cast<unsigned>(idx)); }
        fl::Options o; o.channels = c.ch; o.bitsPerSample = c.bps; o.sampleRate = c.rate; o.blockSize = c.block;
        const Enc e = encodeFile("recm-rt" + std::to_string(idx) + ".flac", o, in, 777);
        CHECK(e.ok);
        const DecInfo d = decode(e.file);
        if (!d.ok) { std::printf("  case %d (%d ch %d bit %u Hz block %d n %zu %s): decoder: %s\n", idx, c.ch, c.bps, c.rate, c.block, c.n, c.sig, d.why.c_str()); }
        CHECK(d.ok);
        if (d.ok) {
            CHECK(d.sampleRate == c.rate);
            CHECK(d.channels == static_cast<unsigned>(c.ch));
            CHECK(d.bps == static_cast<unsigned>(c.bps));
            CHECK(d.total == c.n);
            const bool same = d.pcm == in;
            if (!same) { std::printf("  case %d: decoded samples differ from the input\n", idx); }
            CHECK(same);
            // STREAMINFO MD5 equals the MD5 of the original samples computed independently
            const std::string want = md5OfBytes(pcmBytes(in, c.bps));
            CHECK(hex(d.md5, 16) == want || c.n == 0);
            if (c.n == 0) { CHECK(d.framesDecoded == 0); }
#if defined(_WIN32)
            if (c.n > 0) { std::uint8_t ref[16]; CHECK(cngMd5(pcmBytes(in, c.bps), ref)); CHECK(hex(ref, 16) == hex(d.md5, 16)); }
#endif
            // min/max frame size equal the real frame sizes
            if (d.framesDecoded > 0) {
                std::size_t mn = ~std::size_t{0}, mx = 0;
                for (std::size_t k = 0; k < d.frameStarts.size(); ++k) {
                    const std::size_t end = k + 1 < d.frameStarts.size() ? d.frameStarts[k + 1] : e.file.size();
                    mn = std::min(mn, end - d.frameStarts[k]); mx = std::max(mx, end - d.frameStarts[k]);
                }
                CHECK(d.minFrame == mn);
                CHECK(d.maxFrame == mx);
            }
            const unsigned expectBlock = (c.n > 0 && c.n < static_cast<std::size_t>(c.block)) ? static_cast<unsigned>(std::max<std::size_t>(16, c.n)) : static_cast<unsigned>(c.block);
            CHECK(d.minBlock == expectBlock && d.maxBlock == expectBlock);
        }
        // ffmpeg, if it is here (CRC and MD5 checked by it)
        if (haveFfmpeg() && c.n > 0 && d.ok) {
            Bytes pcm; std::string errText;
            const bool ran = ffmpegDecode(g_dir / ("recm-rt" + std::to_string(idx) + ".flac"), c.bps, &pcm, &errText);
            CHECK(ran);
            if (!errText.empty()) { std::printf("  case %d: ffmpeg said: %s\n", idx, errText.c_str()); }
            CHECK(errText.empty());
            CHECK(pcm == pcmBytes(in, c.bps));
        }
        ++idx;
    }
    std::printf("round trips: %d cases decoded by the test's own decoder%s\n", idx, haveFfmpeg() ? " and by ffmpeg (CRC and MD5 checking on)" : " (ffmpeg not found: not run)");
}

void testExtremes() {
    for (int bps : {16, 24}) {
        const std::int32_t hi = (1 << (bps - 1)) - 1, lo = -(1 << (bps - 1));
        // full-scale square wave, alternating extremes (side channel needs bps+1 bits), L=-R extremes, and DC at both rails
        std::vector<std::int32_t> in;
        for (int i = 0; i < 9000; ++i) { in.push_back(((i / 3) & 1) ? hi : lo); in.push_back(((i / 3) & 1) ? lo : hi); }
        fl::Options o; o.channels = 2; o.bitsPerSample = bps;
        const Enc e = encodeFile("recm-ext" + std::to_string(bps) + ".flac", o, in);
        const DecInfo d = decode(e.file);
        CHECK(e.ok && d.ok);
        CHECK(d.pcm == in);
        std::vector<std::int32_t> dc(8192, lo);
        const Enc e2 = encodeFile("recm-dc" + std::to_string(bps) + ".flac", o, dc);
        const DecInfo d2 = decode(e2.file);
        CHECK(d2.ok && d2.pcm == dc);
        CHECK(d2.constantSub > 0);
        // a few bytes per frame for a constant signal
        CHECK(e2.file.size() < 42 + 4 * 80);
        // random extremes
        std::mt19937 g(5);
        std::vector<std::int32_t> rnd(20000);
        for (auto& s : rnd) { s = (g() & 1) ? hi : lo; }
        const Enc e3 = encodeFile("recm-rx" + std::to_string(bps) + ".flac", o, rnd);
        const DecInfo d3 = decode(e3.file);
        CHECK(d3.ok && d3.pcm == rnd);
        std::printf("extremes %d-bit: square, DC rail and random-rail inputs round-trip; DC frames are constant subframes (%d)\n", bps, d2.constantSub);
    }
}

void testWastedBitsAndStereo() {
    // 16-bit samples that are all multiples of 8: three wasted bits, must be detected and the file smaller
    auto base = sigSpeech(8192, 1, 16, 48000, 3);
    std::vector<std::int32_t> w = base;
    for (auto& s : w) { s = (s / 8) * 8; }
    fl::Options o; o.channels = 1; o.bitsPerSample = 16;
    const Enc e1 = encodeFile("recm-w0.flac", o, base);
    const Enc e2 = encodeFile("recm-w3.flac", o, w);
    const DecInfo d2 = decode(e2.file);
    CHECK(e1.ok && e2.ok && d2.ok);
    CHECK(d2.pcm == w);
    CHECK(d2.wastedSub > 0);
    CHECK(e2.file.size() * 10 < e1.file.size() * 8);   // below 80 % (measured 73 %: three bits of every non-silent sample)
    std::printf("wasted bits: speech-like 8192 samples %zu bytes, the same with 3 low bits zero %zu bytes (%d subframes with wasted bits)\n", e1.file.size(), e2.file.size(), d2.wastedSub);

    // identical channels: side is zero, the writer must pick a decorrelated assignment and about halve the file
    const auto m = sigSpeech(16384, 1, 16, 48000, 4);
    std::vector<std::int32_t> same, indep;
    for (auto s : m) { same.push_back(s); same.push_back(s); }
    indep = sigSpeech(16384, 2, 16, 48000, 4);
    const auto other = sigSpeech(16384, 1, 16, 48000, 99);
    for (std::size_t i = 0; i < indep.size(); i += 2) { indep[i + 1] = other[i / 2]; }
    fl::Options st; st.channels = 2; st.bitsPerSample = 16;
    const Enc es = encodeFile("recm-same.flac", st, same);
    const Enc ei = encodeFile("recm-indep.flac", st, indep);
    const DecInfo ds = decode(es.file), di = decode(ei.file);
    CHECK(ds.ok && di.ok && ds.pcm == same && di.pcm == indep);
    int indepFrames = 0;
    for (int a : ds.assignments) { if (a == 1) { ++indepFrames; } }
    CHECK(indepFrames * 2 < static_cast<int>(ds.assignments.size()));   // a tie (a silent frame) may stay independent; speech frames never do
    fl::Options mono; mono.channels = 1; mono.bitsPerSample = 16;
    const Enc em = encodeFile("recm-mono.flac", mono, m);
    CHECK(es.file.size() < em.file.size() + 400);   // L = R costs about one extra constant-zero side channel, not a second channel
    std::printf("stereo: L=R %zu bytes vs the mono file %zu bytes (assignments used:", es.file.size(), em.file.size());
    for (std::size_t i = 0; i < std::min<std::size_t>(4, ds.assignments.size()); ++i) { std::printf(" %d", ds.assignments[i]); }
    std::printf(")\n");
    // L-side, side-right and mid-side each get chosen on some input and round-trip: a strongly correlated pair with different gains
    bool saw[4] = {false, false, false, false};
    for (int variant = 0; variant < 12; ++variant) {
        std::vector<std::int32_t> v;
        std::mt19937 g(static_cast<unsigned>(variant) + 40);
        const auto a = sigSpeech(8192, 1, 16, 48000, 50 + static_cast<unsigned>(variant));
        for (std::size_t i = 0; i < a.size(); ++i) {
            const int noise = static_cast<int>(g() % 5) - 2;
            std::int32_t L = a[i], R;
            switch (variant % 4) {
                case 0: R = a[i] + noise; break;                       // R = L + small: left-side or mid-side
                case 1: R = static_cast<std::int32_t>(L / 2) + noise; break;
                case 2: L = a[i] + noise; R = a[i] / 3; break;
                default: R = -a[i] + noise; break;
            }
            v.push_back(L); v.push_back(R);
        }
        const Enc ev = encodeFile("recm-sv.flac", st, v);
        const DecInfo dv = decode(ev.file);
        CHECK(ev.ok && dv.ok && dv.pcm == v);
        for (int a2 : dv.assignments) { if (a2 == 1) { saw[0] = true; } else if (a2 == 8) { saw[1] = true; } else if (a2 == 9) { saw[2] = true; } else if (a2 == 10) { saw[3] = true; } }
    }
    std::printf("stereo assignments seen across 12 correlated inputs: independent %d, left-side %d, side-right %d, mid-side %d\n", saw[0], saw[1], saw[2], saw[3]);
    CHECK(saw[3]);
    CHECK(saw[1] || saw[2]);
}

void testSeekTable() {
    const auto in = sigSpeech(100000, 2, 16, 48000, 21);
    fl::Options o; o.channels = 2; o.bitsPerSample = 16; o.seekPoints = 10;
    const Enc e = encodeFile("recm-seek.flac", o, in);
    const DecInfo d = decode(e.file);
    CHECK(e.ok && d.ok && d.pcm == in && d.hasSeek);
    CHECK(d.seek.size() == 10);
    int real = 0;
    std::uint64_t prev = 0; bool sorted = true;
    for (std::size_t i = 0; i < d.seek.size(); ++i) {
        const auto& s = d.seek[i];
        if (s.sample == ~0ull) { continue; }
        ++real;
        if (i > 0 && s.sample <= prev) { sorted = false; }
        prev = s.sample;
        // the offset must land exactly on a frame header whose first sample is the stated one (frame number * block size)
        const std::size_t at = d.firstFrameAt + static_cast<std::size_t>(s.offset);
        CHECK(at + 4 < e.file.size() && e.file[at] == 0xFF && (e.file[at + 1] & 0xFE) == 0xF8);
        bool found = false;
        for (std::size_t k = 0; k < d.frameStarts.size(); ++k) { if (d.frameStarts[k] == at) { found = (k * 4096ull == s.sample); } }
        CHECK(found);
        CHECK(s.n == 4096 || s.sample + s.n == 100000);
    }
    CHECK(real == 10);
    CHECK(sorted);
    // fewer frames than points: placeholders last
    const auto small = sigSine(5000, 2, 16, 48000, 0.3);
    const Enc e2 = encodeFile("recm-seek2.flac", o, small);
    const DecInfo d2 = decode(e2.file);
    CHECK(d2.ok && d2.seek.size() == 10);
    int ph = 0, rs = 0; bool placeholdersLast = true, seenPh = false;
    for (const auto& s : d2.seek) { if (s.sample == ~0ull) { ++ph; seenPh = true; } else { ++rs; if (seenPh) { placeholdersLast = false; } } }
    CHECK(rs == 2 && ph == 8 && placeholdersLast);
    if (haveFfmpeg()) {   // a file with a seek table must still decode under ffmpeg
        Bytes pcm; std::string errText;
        CHECK(ffmpegDecode(g_dir / "recm-seek.flac", 16, &pcm, &errText));
        CHECK(errText.empty() && pcm == pcmBytes(in, 16));
    }
    std::printf("seek table: 10 points on 100000 frames, every offset lands on a frame header with the stated first sample; 5000 frames leave 8 placeholders last\n");
}

void testCrashContract() {
    const auto in = sigSpeech(20000, 1, 16, 48000, 31);
    fl::Options o; o.channels = 1; o.bitsPerSample = 16; o.seekPoints = 4;
    const std::string path = (g_dir / "recm-crash.flac").string();
    fl::Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    CHECK(w.write(in.data(), 16384, &err));            // four whole blocks, no finish
    const Bytes snap = readFile(path);                  // what a crash would leave
    const DecInfo d = decode(snap);
    CHECK(d.ok);
    CHECK(d.total == 0 && d.md5Zero);                  // unknown, as the RFC allows
    CHECK(d.framesDecoded == 4);
    CHECK(std::vector<std::int32_t>(in.begin(), in.begin() + 16384) == d.pcm);
    // the same snapshot decodes in ffmpeg without a CRC complaint
    if (haveFfmpeg()) {
        fs::path p = g_dir / "recm-crash-snap.flac";
        { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(snap.data()), static_cast<std::streamsize>(snap.size())); }
        Bytes pcm; std::string et;
        CHECK(ffmpegDecode(p, 16, &pcm, &et));
        CHECK(pcm.size() == 16384 * 2);
    }
    CHECK(w.write(in.data() + 16384, 3616, &err));
    CHECK(w.finish(&err));
    const DecInfo d2 = decode(readFile(path));
    CHECK(d2.ok && d2.total == 20000 && !d2.md5Zero && d2.pcm == in);
    std::printf("crash contract: a file snapshotted after 4 blocks without finish() decodes 16384 samples with total 0 / MD5 0; after finish() total 20000 and MD5 set\n");
}

void testRefusals() {
    fl::Writer w;
    std::string err;
    fl::Options o;
    o.channels = 0; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err) && !err.empty()); o.channels = 3; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.bitsPerSample = 8; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.bitsPerSample = 32; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.sampleRate = 0; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.sampleRate = 1048576; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.blockSize = 100; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.blockSize = 65536; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.seekPoints = -1; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    o = fl::Options{}; o.seekPoints = 5000; CHECK(!w.open((g_dir / "recm-bad.flac").string(), o, &err));
    CHECK(!w.isOpen());
    o = fl::Options{};
    CHECK(!w.open((g_dir / "no-such-dir" / "x.flac").string(), o, &err));   // folder missing: refused, no throw
    CHECK(err.find("cannot create") != std::string::npos);
    CHECK(!w.write(nullptr, 0, &err));                                        // not open
    CHECK(w.finish(&err));                                                    // idempotent when not open
    // double open
    CHECK(w.open((g_dir / "recm-dbl.flac").string(), o, &err));
    CHECK(!w.open((g_dir / "recm-dbl2.flac").string(), o, &err));
    CHECK(w.finish(&err));
    // zero frames written: a valid file with no frames
    const DecInfo d = decode(readFile(g_dir / "recm-dbl.flac"));
    CHECK(d.ok && d.framesDecoded == 0 && d.total == 0);
    // writing 0 frames is a no-op
    CHECK(w.open((g_dir / "recm-zero.flac").string(), o, &err));
    CHECK(w.write(nullptr, 0, &err));
    CHECK(w.finish(&err));
}

// Decoder hardening: a corrupted file must be REFUSED by the independent decoder (proves its CRC checks actually bite, so the round trips mean something).
void testDecoderBites() {
    const auto in = sigSpeech(9000, 2, 16, 48000, 61);
    fl::Options o; o.channels = 2; o.bitsPerSample = 16;
    Enc e = encodeFile("recm-bite.flac", o, in);
    CHECK(decode(e.file).ok);
    int refused = 0, tried = 0;
    std::mt19937 g(9);
    for (int i = 0; i < 300; ++i) {
        Bytes b = e.file;
        const std::size_t at = 42 + g() % (b.size() - 42);
        b[at] ^= static_cast<std::uint8_t>(1u << (g() % 8));
        ++tried;
        if (!decode(b).ok) { ++refused; }
    }
    CHECK(refused == tried);   // a single flipped bit in any frame is caught by CRC-8 or CRC-16 (or an earlier structural check)
    std::printf("decoder self-check: %d of %d single-bit corruptions refused\n", refused, tried);
}

struct Ratio { std::size_t bytes; double pct; };
Ratio ratioOf(const std::string& name, const fl::Options& o, const std::vector<std::int32_t>& in) {
    const Enc e = encodeFile(name, o, in);
    const DecInfo d = decode(e.file);
    CHECK(e.ok && d.ok && d.pcm == in);
    const double raw = static_cast<double>(in.size()) * (o.bitsPerSample / 8.0);
    return {e.file.size(), 100.0 * static_cast<double>(e.file.size()) / raw};
}

void testCompression() {
    fl::Options o; o.channels = 1; o.bitsPerSample = 16; o.sampleRate = 48000;
    const std::size_t n = 48000 * 10;
    const Ratio speech = ratioOf("recm-c-speech.flac", o, sigSpeech(n, 1, 16, 48000, 71));
    const Ratio noiseFull = ratioOf("recm-c-noise.flac", o, sigNoise(n, 1, 16, 1.0, 72));
    const Ratio noiseLow = ratioOf("recm-c-noise2.flac", o, sigNoise(n, 1, 16, 0.0625, 73));   // +-2048: 12 bits of entropy
    const Ratio sine = ratioOf("recm-c-sine.flac", o, sigSine(n, 1, 16, 48000, 0.5));
    std::vector<std::int32_t> zeros(n, 0);
    const Ratio silence = ratioOf("recm-c-zero.flac", o, zeros);
    std::printf("compression (10 s mono 16-bit, 48 kHz, size as %% of the raw PCM): speech-like %.1f %%, full-scale white noise %.2f %%, +-2048 noise %.1f %%, sine 0.5 %.1f %%, silence %.3f %%\n",
                speech.pct, noiseFull.pct, noiseLow.pct, sine.pct, silence.pct);
    // PINS (set from the measured values printed above, with margin)
    CHECK(speech.pct < 60.0 && speech.pct > 10.0);
    CHECK(noiseFull.pct < 100.5);               // incompressible: verbatim subframes plus ~16 bytes of frame overhead per 4096 samples
    CHECK(noiseFull.pct > 99.0);
    CHECK(noiseLow.pct < 79.5 && noiseLow.pct > 74.0);   // uniform +-2048: 12 bits of 16 = 75 % entropy; Rice coding of a uniform value costs a little more (measured 78.2 %)
    CHECK(sine.pct < 25.0);   // measured 22.5 %: the 16-bit rounding noise, amplified by the order-4 predictor, sets the floor
    CHECK(silence.pct < 0.2);
    // 24-bit stereo speech-like
    fl::Options o24; o24.channels = 2; o24.bitsPerSample = 24;
    const Ratio sp24 = ratioOf("recm-c-s24.flac", o24, sigSpeech(48000 * 5, 2, 24, 48000, 74));
    std::printf("compression 5 s stereo 24-bit speech-like (L=R): %.1f %%\n", sp24.pct);
    CHECK(sp24.pct < 40.0);
    // against ffmpeg's own FLAC encoder, as an outside yardstick (informational plus a loose pin)
    if (haveFfmpeg()) {
        const auto in = sigSpeech(n, 1, 16, 48000, 71);
        const fs::path wav = g_dir / "recm-yard.pcm";
        { std::ofstream f(wav, std::ios::binary); const Bytes pb = pcmBytes(in, 16); f.write(reinterpret_cast<const char*>(pb.data()), static_cast<std::streamsize>(pb.size())); }
        for (int level : {0, 5}) {
            const fs::path out = g_dir / ("recm-yard" + std::to_string(level) + ".flac");
            const int rc = runCmd("ffmpeg -nostdin -v error -y -f s16le -ar 48000 -ac 1 -i \"" + wav.string() + "\" -c:a flac -compression_level " + std::to_string(level) + " \"" + out.string() + "\"");
            if (rc == 0) {
                const double pct = 100.0 * static_cast<double>(fs::file_size(out)) / (static_cast<double>(n) * 2.0);
                std::printf("  yardstick: ffmpeg's FLAC encoder level %d gives %.1f %% on the same speech-like input (this writer %.1f %%)\n", level, pct, speech.pct);
                if (level == 5) { CHECK(speech.pct < pct * 1.10); }
            }
        }
    }
}

void testSpeed() {
    // encoding cost: seconds of audio per second of CPU (the encoder runs on a worker, never the audio thread)
    const auto in = sigSpeech(48000 * 10, 2, 16, 48000, 81);
    fl::Options o; o.channels = 2; o.bitsPerSample = 16;
    const auto t0 = std::chrono::steady_clock::now();
    const Enc e = encodeFile("recm-speed.flac", o, in, 4096);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(e.ok);
    std::printf("speed: 10 s of 48 kHz stereo 16-bit speech-like encoded in %.3f s (%.0fx real time, including disk)\n", s, 10.0 / s);
    CHECK(s < 5.0);   // 2x real time is the floor for a recorder; the measured value is far inside it
}

void testAsync() {
    // 1) a sine through AsyncRecorder from a producer thread in 480-frame pushes, a non-blocking push, and the worker is another thread
    fl::AsyncRecorder rec;
    fl::Options o; o.channels = 2; o.bitsPerSample = 16; o.sampleRate = 48000;
    std::string err;
    const std::string path = (g_dir / "recm-async.flac").string();
    CHECK(rec.start(path, o, &err));
    CHECK(rec.running());
    CHECK(!rec.start(path, o, &err));   // second start refused
    const std::size_t total = 48000 * 6;
    std::vector<float> src(total * 2);
    for (std::size_t i = 0; i < total; ++i) {
        const float a = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 440.0 * static_cast<double>(i) / 48000.0));
        src[2 * i] = a; src[2 * i + 1] = -a * 0.7f;
    }
    // hostile values: NaN, +-inf, beyond full scale
    src[10] = std::nanf(""); src[11] = INFINITY; src[12] = -INFINITY; src[13] = 3.0f; src[14] = -3.0f; src[15] = 1.0f;
    double maxPushUs = 0.0;
    long newsDuringPush = 0;
    std::thread producer([&] {
        for (std::size_t at = 0; at < total; at += 480) {
            const std::size_t n = std::min<std::size_t>(480, total - at);
            const auto t0 = std::chrono::steady_clock::now();
            {
                AllocProbe probe;
                rec.push(src.data() + at * 2, n);
                newsDuringPush += probe.news();
            }
            maxPushUs = std::max(maxPushUs, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            if ((at / 480) % 16 == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); }   // about 10 ms of audio per 3 ms: faster than real time
        }
    });
    producer.join();
    CHECK(rec.stop(&err));
    CHECK(!rec.running());
    CHECK(rec.droppedFrames() == 0);
    CHECK(rec.acceptedFrames() == total);
    CHECK(newsDuringPush == 0);
    CHECK(maxPushUs < 3000.0);
    const DecInfo d = decode(readFile(path));
    CHECK(d.ok && d.total == total);
    // expected = what the converter must give: round(v * 32768), clamped, non-finite = 0
    bool same = d.ok && d.pcm.size() == total * 2;
    std::size_t firstBad = 0;
    for (std::size_t i = 0; same && i < src.size(); ++i) {
        const float v = src[i];
        const std::int32_t want = std::isfinite(v) ? clampTo(static_cast<double>(v) * 32768.0, 16) : 0;
        if (d.pcm[i] != want) { same = false; firstBad = i; }
    }
    if (!same) { std::printf("  async: first difference at sample %zu\n", firstBad); }
    CHECK(same);
    CHECK(d.ok && d.pcm[11] == 0 && d.pcm[12] == 0 && d.pcm[13] == 32767 && d.pcm[14] == -32768 && d.pcm[15] == 32767);
    std::printf("async: 6 s stereo through the recorder from a producer thread: 0 dropped, 0 allocations in %zu push calls, slowest push %.1f us, file decodes to the expected quantisation (NaN and inf = 0, clamped at the rails)\n",
                total / 480, maxPushUs);

    // 2) a ring too small for a burst: frames are dropped and counted, push never blocks, the file is still valid and holds exactly the accepted frames
    fl::AsyncRecorder small;
    const std::string p2 = (g_dir / "recm-async-drop.flac").string();
    fl::Options m; m.channels = 1; m.bitsPerSample = 16;
    CHECK(small.start(p2, m, &err, 1024));
    std::vector<float> burst(200000, 0.25f);
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t took = small.push(burst.data(), burst.size());
    const double pushMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took <= 1024);
    CHECK(small.droppedFrames() == burst.size() - took);
    CHECK(small.acceptedFrames() == took);
    CHECK(small.stop(&err));
    const DecInfo d2 = decode(readFile(p2));
    CHECK(d2.ok && d2.total == took);
    std::printf("async overflow: a 200000-frame burst into a 1024-frame ring accepted %zu, dropped %llu, push returned in %.3f ms\n", took,
                static_cast<unsigned long long>(small.droppedFrames()), pushMs);
    // push after stop is a no-op returning 0
    CHECK(small.push(burst.data(), 10) == 0);
    CHECK(small.stop(&err));   // idempotent

    // 3) start in a missing folder is refused
    fl::AsyncRecorder bad;
    CHECK(!bad.start((g_dir / "nope" / "x.flac").string(), m, &err));
    CHECK(!bad.running());
    CHECK(bad.stop(&err));
    // 4) restart works (a second recording with the same object)
    CHECK(small.start((g_dir / "recm-async-2.flac").string(), m, &err));
    CHECK(small.push(burst.data(), 100) == 100);
    CHECK(small.stop(&err));
    CHECK(decode(readFile(g_dir / "recm-async-2.flac")).total == 100);
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / "recm-flac";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    testMd5();
    testRoundTrips();
    testExtremes();
    testWastedBitsAndStereo();
    testSeekTable();
    testCrashContract();
    testRefusals();
    testDecoderBites();
    testCompression();
    testSpeed();
    testAsync();
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    return testSummary("test_flac_writer");
}
