// FLAC writer for audio recordings: 16- and 24-bit, mono and stereo, fixed predictors (orders 0..4) with partitioned Rice coding, constant and
// verbatim subframes, wasted-bits detection, stereo decorrelation (independent / left-side / side-right / mid-side chosen per frame by exact
// coded size), a correct STREAMINFO (min/max frame size, total samples, MD5) and an optional SEEKTABLE.
//
// STANDALONE BLOCK. Nothing here is wired into the engine or the recorder (docs/feature-records/recording-models.md, "Part 1", has the hand-off).
// Written from the format specification only (clean room): RFC 9639 "Free Lossless Audio Codec (FLAC)", https://www.rfc-editor.org/rfc/rfc9639.html
// (read 2026-11-11): sections 4.2 (stereo), 6 and 8 (metadata blocks: STREAMINFO, SEEKTABLE), 9 (frame header, subframes, fixed predictors, residual
// coding), CRC-8 polynomial 0x07 and CRC-16 polynomial 0x8005 both initialised to 0, MD5 per RFC 1321 over the interleaved little-endian signed
// samples sign-extended to whole bytes. No FLAC library or program source was read.
//
// THREADS
//   Writer          one thread (whoever calls it). Blocking file I/O: never the audio thread.
//   AsyncRecorder   push() is called by ONE producing thread (the audio / pipeline thread): wait-free, allocation-free, copies floats into a
//                   fixed single-producer single-consumer ring and returns; a worker thread converts, encodes and writes. A full ring DROPS the
//                   frames that do not fit and counts them (droppedFrames()); nothing ever blocks the producer.
//
// CRASH CONTRACT. open() writes "fLaC", a STREAMINFO with total samples 0 (= unknown) and MD5 0 (= unknown), both legal per the RFC, and a seek table of
// placeholders, then flushes. A process that dies mid-recording leaves a file every FLAC reader can play; finish() patches the true values.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace cascade::core::flac {

// ---------------------------------------------------------------------------------------------------------------------------------------------
// MD5 (RFC 1321), streaming.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class Md5 {
public:
    Md5() { reset(); }
    void reset() {
        a_ = 0x67452301u; b_ = 0xefcdab89u; c_ = 0x98badcfeu; d_ = 0x10325476u;
        len_ = 0; fill_ = 0;
    }
    void update(const std::uint8_t* p, std::size_t n) {
        len_ += n;
        while (n > 0) {
            const std::size_t take = std::min<std::size_t>(64 - fill_, n);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    std::array<std::uint8_t, 16> finish() {
        const std::uint64_t bits = len_ * 8;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (fill_ != 56) { update(&zero, 1); }
        std::uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) { lenb[i] = static_cast<std::uint8_t>(bits >> (8 * i)); }
        update(lenb, 8);
        std::array<std::uint8_t, 16> out{};
        const std::uint32_t w[4] = {a_, b_, c_, d_};
        for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) { out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(w[i] >> (8 * j)); } }
        return out;
    }
private:
    static std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const std::uint8_t* p) {
        static const std::uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(p[i * 4]) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
        }
        std::uint32_t a = a_, b = b_, c = c_, d = d_;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
            else { f = c ^ (b | ~d); g = (7 * i) & 15; }
            const std::uint32_t t = d; d = c; c = b;
            b = b + rotl(a + f + K[i] + m[g], S[i]);
            a = t;
        }
        a_ += a; b_ += b; c_ += c; d_ += d;
    }
    std::uint32_t a_, b_, c_, d_;
    std::uint64_t len_;
    std::uint8_t buf_[64];
    std::size_t fill_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Options.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct Options {
    std::uint32_t sampleRate = 48000;     // 1 .. 1048575 (20 bits in STREAMINFO)
    int channels = 1;                     // 1 or 2
    int bitsPerSample = 16;               // 16 or 24
    int blockSize = 4096;                 // 256 .. 32768, fixed-size blocks (the last may be shorter)
    int seekPoints = 0;                   // 0 = no SEEKTABLE; otherwise the number of entries reserved (placeholders until finish())
};

namespace detail {

inline std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
    std::uint8_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) { c = static_cast<std::uint8_t>((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1)); }
    }
    return c;
}
inline const std::array<std::uint16_t, 256>& crc16Table() {
    static const std::array<std::uint16_t, 256> t = [] {
        std::array<std::uint16_t, 256> r{};
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i << 8;
            for (int b = 0; b < 8; ++b) { c = (c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1); }
            r[i] = static_cast<std::uint16_t>(c & 0xFFFF);
        }
        return r;
    }();
    return t;
}
inline std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    const auto& t = crc16Table();
    std::uint16_t c = 0;
    for (std::size_t i = 0; i < n; ++i) { c = static_cast<std::uint16_t>((c << 8) ^ t[((c >> 8) ^ p[i]) & 0xFF]); }
    return c;
}

// MSB-first bit sink.
class BitWriter {
public:
    void clear() { bytes_.clear(); acc_ = 0; nacc_ = 0; }
    std::size_t bitCount() const { return bytes_.size() * 8 + static_cast<std::size_t>(nacc_); }
    void put(std::uint64_t v, int n) {    // n in 0..32
        for (int i = n - 1; i >= 0; --i) { putBit(static_cast<unsigned>((v >> i) & 1u)); }
    }
    void putBit(unsigned b) {
        acc_ = static_cast<std::uint8_t>((acc_ << 1) | (b & 1u));
        if (++nacc_ == 8) { bytes_.push_back(acc_); acc_ = 0; nacc_ = 0; }
    }
    void putSigned(std::int64_t v, int n) { put(static_cast<std::uint64_t>(v) & ((1ull << n) - 1), n); }
    void putUnaryZerosThenOne(std::uint64_t q) {
        for (std::uint64_t i = 0; i < q; ++i) { putBit(0); }
        putBit(1);
    }
    void alignZero() { while (nacc_ != 0) { putBit(0); } }
    void append(const BitWriter& o) {
        for (std::uint8_t byte : o.bytes_) { put(byte, 8); }
        for (int i = o.nacc_ - 1; i >= 0; --i) { putBit((o.acc_ >> i) & 1u); }
    }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }   // only whole bytes: call alignZero() first
private:
    std::vector<std::uint8_t> bytes_;
    std::uint8_t acc_ = 0;
    int nacc_ = 0;
};

inline std::uint64_t zigzag(std::int64_t v) { return v >= 0 ? static_cast<std::uint64_t>(v) << 1 : (static_cast<std::uint64_t>(-v) << 1) - 1; }

// The RFC's UTF-8-like coded number (frame number here), up to 36 bits.
inline void putUtf8Number(std::vector<std::uint8_t>& out, std::uint64_t v) {
    if (v < 0x80) { out.push_back(static_cast<std::uint8_t>(v)); return; }
    int extra = 1;
    while (extra < 6 && v >= (1ull << (5 * extra + 6))) { ++extra; }
    const std::uint8_t lead = static_cast<std::uint8_t>((0xFF00u >> (extra + 1)) & 0xFF);
    out.push_back(static_cast<std::uint8_t>(lead | (v >> (6 * extra))));
    for (int i = extra - 1; i >= 0; --i) { out.push_back(static_cast<std::uint8_t>(0x80 | ((v >> (6 * i)) & 0x3F))); }
}

inline int sampleRateCode(std::uint32_t r, std::uint32_t* extra, int* extraBytes) {
    *extraBytes = 0; *extra = 0;
    switch (r) {
        case 88200: return 1; case 176400: return 2; case 192000: return 3; case 8000: return 4; case 16000: return 5; case 22050: return 6;
        case 24000: return 7; case 32000: return 8; case 44100: return 9; case 48000: return 10; case 96000: return 11; default: break;
    }
    if (r % 1000 == 0 && r / 1000 <= 255) { *extra = r / 1000; *extraBytes = 1; return 12; }
    if (r <= 65535) { *extra = r; *extraBytes = 2; return 13; }
    if (r % 10 == 0 && r / 10 <= 65535) { *extra = r / 10; *extraBytes = 2; return 14; }
    return 0;   // taken from STREAMINFO
}

// Encodes one channel's samples as a subframe (header included).
class SubframeEncoder {
public:
    // Returns the size in bits of the best subframe for x[0..n); writes it to *bw when bw != nullptr. `bps` is the effective depth (side: depth + 1).
    std::size_t encode(const std::int32_t* x, int n, int bps, BitWriter* bw) {
        std::uint32_t orAll = 0;
        for (int i = 0; i < n; ++i) { orAll |= static_cast<std::uint32_t>(x[i]); }
        int wasted = 0;
        if (orAll != 0) { while (((orAll >> wasted) & 1u) == 0) { ++wasted; } }
        const std::int32_t* s = x;
        if (wasted > 0) {
            shifted_.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) { shifted_[static_cast<std::size_t>(i)] = x[i] >> wasted; }
            s = shifted_.data();
        }
        const int eb = bps - wasted;
        const std::size_t headerBits = 8 + static_cast<std::size_t>(wasted);   // pad + 6 type + flag + unary(wasted - 1) (= wasted bits)

        bool allSame = n > 0;
        for (int i = 1; i < n; ++i) { if (s[i] != s[0]) { allSame = false; break; } }
        if (allSame) {
            if (bw != nullptr) { writeHeader(*bw, 0, wasted); bw->putSigned(s[0], eb); }
            return headerBits + static_cast<std::size_t>(eb);
        }
        std::size_t best = headerBits + static_cast<std::size_t>(n) * static_cast<std::size_t>(eb);   // verbatim
        int bestOrder = -1;
        Plan bestPlan;
        for (int order = 0; order <= 4 && order < n; ++order) {
            Plan plan;
            if (!plan_(s, n, order, &plan)) { continue; }
            const std::size_t bits = headerBits + static_cast<std::size_t>(order) * static_cast<std::size_t>(eb) + 2 + 4 + plan.bits;
            if (bits < best) { best = bits; bestOrder = order; bestPlan = plan; }
        }
        if (bw != nullptr) {
            if (bestOrder < 0) {
                writeHeader(*bw, 1, wasted);
                for (int i = 0; i < n; ++i) { bw->putSigned(s[i], eb); }
            } else {
                writeHeader(*bw, 8 + bestOrder, wasted);
                for (int i = 0; i < bestOrder; ++i) { bw->putSigned(s[i], eb); }
                residual(s, n, bestOrder, bestPlan, *bw);
            }
        }
        return best;
    }

private:
    struct Plan {
        int partOrder = 0;
        bool wide = false;                   // 5-bit parameters
        std::array<std::uint8_t, 256> k{};   // up to 2^8 partitions
        std::size_t bits = 0;                // residual payload bits including the parameters
    };

    static void writeHeader(BitWriter& bw, int type, int wasted) {
        bw.putBit(0);
        bw.put(static_cast<std::uint64_t>(type), 6);
        if (wasted > 0) { bw.putBit(1); for (int i = 0; i < wasted - 1; ++i) { bw.putBit(0); } bw.putBit(1); } else { bw.putBit(0); }
    }

    bool computeResidual(const std::int32_t* s, int n, int order) {
        res_.resize(static_cast<std::size_t>(n));
        for (int i = order; i < n; ++i) {
            std::int64_t p = 0;
            switch (order) {
                case 0: p = 0; break;
                case 1: p = s[i - 1]; break;
                case 2: p = 2 * static_cast<std::int64_t>(s[i - 1]) - s[i - 2]; break;
                case 3: p = 3 * static_cast<std::int64_t>(s[i - 1]) - 3 * static_cast<std::int64_t>(s[i - 2]) + s[i - 3]; break;
                default: p = 4 * static_cast<std::int64_t>(s[i - 1]) - 6 * static_cast<std::int64_t>(s[i - 2]) + 4 * static_cast<std::int64_t>(s[i - 3]) - s[i - 4]; break;
            }
            const std::int64_t r = static_cast<std::int64_t>(s[i]) - p;
            if (r >= (1ll << 31) || r <= -(1ll << 31)) { return false; }
            res_[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(r);
        }
        return true;
    }

    static std::size_t partitionBits(const std::uint64_t* u, int count, int k) {
        std::size_t bits = static_cast<std::size_t>(count) * static_cast<std::size_t>(k + 1);
        for (int i = 0; i < count; ++i) { bits += static_cast<std::size_t>(u[i] >> k); }
        return bits;
    }

    bool plan_(const std::int32_t* s, int n, int order, Plan* out) {
        if (!computeResidual(s, n, order)) { return false; }
        folded_.resize(static_cast<std::size_t>(n));
        for (int i = order; i < n; ++i) { folded_[static_cast<std::size_t>(i)] = zigzag(res_[static_cast<std::size_t>(i)]); }
        bool have = false;
        for (int po = 0; po <= 8; ++po) {
            if ((n % (1 << po)) != 0) { break; }
            const int psize = n >> po;
            if (psize <= order) { break; }
            Plan plan;
            plan.partOrder = po;
            std::size_t total = 0;
            int maxK = 0;
            for (int p = 0; p < (1 << po); ++p) {
                const int first = p == 0 ? order : p * psize;
                const int cnt = (p + 1) * psize - first;
                const std::uint64_t* u = folded_.data() + first;
                std::uint64_t sum = 0;
                for (int i = 0; i < cnt; ++i) { sum += u[i]; }
                int est = 0;
                if (cnt > 0) { const std::uint64_t mean = sum / static_cast<std::uint64_t>(cnt); while (est < 30 && (mean >> (est + 1)) != 0) { ++est; } }
                std::size_t bestBits = ~std::size_t{0};
                int bestK = 0;
                for (int k = std::max(0, est - 1); k <= std::min(30, est + 1); ++k) {
                    const std::size_t b = partitionBits(u, cnt, k);
                    if (b < bestBits) { bestBits = b; bestK = k; }
                }
                plan.k[static_cast<std::size_t>(p)] = static_cast<std::uint8_t>(bestK);
                maxK = std::max(maxK, bestK);
                total += bestBits;
            }
            plan.wide = maxK > 14;
            plan.bits = total + static_cast<std::size_t>(1 << po) * (plan.wide ? 5u : 4u);
            if (!have || plan.bits < out->bits) { *out = plan; have = true; }
        }
        return have;
    }

    void residual(const std::int32_t* s, int n, int order, const Plan& plan, BitWriter& bw) {
        computeResidual(s, n, order);
        bw.put(plan.wide ? 1 : 0, 2);
        bw.put(static_cast<std::uint64_t>(plan.partOrder), 4);
        const int psize = n >> plan.partOrder;
        for (int p = 0; p < (1 << plan.partOrder); ++p) {
            const int k = plan.k[static_cast<std::size_t>(p)];
            bw.put(static_cast<std::uint64_t>(k), plan.wide ? 5 : 4);
            const int first = p == 0 ? order : p * psize;
            for (int i = first; i < (p + 1) * psize; ++i) {
                const std::uint64_t u = zigzag(res_[static_cast<std::size_t>(i)]);
                bw.putUnaryZerosThenOne(u >> k);
                if (k > 0) { bw.put(u & ((1ull << k) - 1), k); }
            }
        }
    }

    std::vector<std::int32_t> shifted_, res_;
    std::vector<std::uint64_t> folded_;
};

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Writer.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class Writer {
public:
    ~Writer() { if (open_) { finish(nullptr); } }

    // Creates (truncating) the file and writes the stream header. False with *err on refusal.
    bool open(const std::string& path, const Options& opt, std::string* err) {
        if (open_) { return fail(err, "already open"); }
        if (opt.channels < 1 || opt.channels > 2) { return fail(err, "channels must be 1 or 2"); }
        if (opt.bitsPerSample != 16 && opt.bitsPerSample != 24) { return fail(err, "bits per sample must be 16 or 24"); }
        if (opt.sampleRate < 1 || opt.sampleRate > 1048575) { return fail(err, "sample rate out of range"); }
        if (opt.blockSize < 256 || opt.blockSize > 32768) { return fail(err, "block size must be 256..32768"); }
        if (opt.seekPoints < 0 || opt.seekPoints > 4096) { return fail(err, "seek points must be 0..4096"); }
        opt_ = opt;
        file_.clear();
        file_.open(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!file_) { return fail(err, "cannot create " + path); }
        pending_.clear();
        pending_.reserve(static_cast<std::size_t>(opt_.blockSize) * static_cast<std::size_t>(opt_.channels));
        md5_.reset(); frameNo_ = 0; totalFrames_ = 0; minFrame_ = 0; maxFrame_ = 0; index_.clear(); firstFrameOffset_ = 0;
        shortOnly_ = false; lastBlock_ = 0; failed_ = false; error_.clear();
        std::vector<std::uint8_t> hdr = {'f', 'L', 'a', 'C'};
        appendMetaHeader(hdr, 0, opt_.seekPoints == 0, 34);
        hdr.resize(hdr.size() + 34, 0);
        streamInfoAt_ = 8;
        fillStreamInfo(&hdr[streamInfoAt_], true);
        if (opt_.seekPoints > 0) {
            appendMetaHeader(hdr, 3, true, static_cast<std::uint32_t>(opt_.seekPoints) * 18u);
            seekTableAt_ = hdr.size();
            for (int i = 0; i < opt_.seekPoints; ++i) { for (int b = 0; b < 8; ++b) { hdr.push_back(0xFF); } for (int b = 0; b < 10; ++b) { hdr.push_back(0); } }
        }
        file_.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        file_.flush();
        if (!file_) { file_.close(); return fail(err, "write failed (disk full?)"); }
        firstFrameOffset_ = hdr.size();
        bytesWritten_ = hdr.size();
        open_ = true;
        return true;
    }

    // `frames` interleaved sample frames (channels per frame), each value within the bit depth. False on a write failure (the writer then refuses
    // further writes; finish() still closes the file).
    bool write(const std::int32_t* interleaved, std::size_t frames, std::string* err = nullptr) {
        if (!open_) { return fail(err, "not open"); }
        if (failed_) { return fail(err, error_); }
        const std::size_t ch = static_cast<std::size_t>(opt_.channels);
        const std::size_t perBlock = static_cast<std::size_t>(opt_.blockSize) * ch;
        std::size_t i = 0;
        while (i < frames) {
            const std::size_t room = (perBlock - pending_.size()) / ch;
            const std::size_t take = std::min(room, frames - i);
            pending_.insert(pending_.end(), interleaved + i * ch, interleaved + (i + take) * ch);
            i += take;
            if (pending_.size() == perBlock) { if (!flushBlock()) { return fail(err, error_); } }
        }
        return true;
    }

    // Writes the last partial block, patches STREAMINFO (and the seek table) and closes. Idempotent.
    bool finish(std::string* err = nullptr) {
        if (!open_) { return true; }
        if (!failed_ && !pending_.empty()) { flushBlock(); }
        if (!failed_) {
            std::vector<std::uint8_t> si(34, 0);
            fillStreamInfo(si.data(), false);
            file_.seekp(static_cast<std::streamoff>(streamInfoAt_));
            file_.write(reinterpret_cast<const char*>(si.data()), 34);
            if (opt_.seekPoints > 0) {
                std::vector<std::uint8_t> st;
                const std::size_t nF = index_.size();
                const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(opt_.seekPoints), nF);
                for (std::size_t k = 0; k < want; ++k) {
                    const FrameRec& f = index_[k * nF / want];
                    put64(st, f.firstSample); put64(st, f.offset);
                    st.push_back(static_cast<std::uint8_t>(f.samples >> 8)); st.push_back(static_cast<std::uint8_t>(f.samples));
                }
                for (std::size_t k = want; k < static_cast<std::size_t>(opt_.seekPoints); ++k) {
                    for (int b = 0; b < 8; ++b) { st.push_back(0xFF); }
                    for (int b = 0; b < 10; ++b) { st.push_back(0); }
                }
                file_.seekp(static_cast<std::streamoff>(seekTableAt_));
                file_.write(reinterpret_cast<const char*>(st.data()), static_cast<std::streamsize>(st.size()));
            }
        }
        file_.flush();
        if (!file_ && !failed_) { failed_ = true; error_ = "write failed"; }
        file_.close();
        open_ = false;
        if (failed_) { return fail(err, error_); }
        return true;
    }

    bool isOpen() const { return open_; }
    std::uint64_t framesWritten() const { return totalFrames_ + pending_.size() / static_cast<std::size_t>(opt_.channels); }
    std::uint64_t bytesWritten() const { return bytesWritten_; }
    const std::string& lastError() const { return error_; }

    // Encodes one frame of `n` interleaved sample frames. Exposed so tests can look at single frames.
    void encodeFrame(const std::int32_t* inter, int n, std::uint64_t frameNumber, std::vector<std::uint8_t>& out) {
        const int ch = opt_.channels;
        const int bps = opt_.bitsPerSample;
        std::vector<std::uint8_t> h = {0xFF, 0xF8};
        int bsCode = 6; int bsExtraBytes = 1;
        switch (n) {
            case 192: bsCode = 1; bsExtraBytes = 0; break;
            case 576: bsCode = 2; bsExtraBytes = 0; break; case 1152: bsCode = 3; bsExtraBytes = 0; break;
            case 2304: bsCode = 4; bsExtraBytes = 0; break; case 4608: bsCode = 5; bsExtraBytes = 0; break;
            case 256: bsCode = 8; bsExtraBytes = 0; break; case 512: bsCode = 9; bsExtraBytes = 0; break; case 1024: bsCode = 10; bsExtraBytes = 0; break;
            case 2048: bsCode = 11; bsExtraBytes = 0; break; case 4096: bsCode = 12; bsExtraBytes = 0; break; case 8192: bsCode = 13; bsExtraBytes = 0; break;
            case 16384: bsCode = 14; bsExtraBytes = 0; break; case 32768: bsCode = 15; bsExtraBytes = 0; break;
            default: bsCode = n <= 256 ? 6 : 7; bsExtraBytes = n <= 256 ? 1 : 2; break;
        }
        std::uint32_t srExtra; int srBytes;
        const int srCode = detail::sampleRateCode(opt_.sampleRate, &srExtra, &srBytes);
        h.push_back(static_cast<std::uint8_t>((bsCode << 4) | srCode));

        int assign = ch - 1;
        detail::BitWriter sub[2];
        if (ch == 1) {
            chan_[0].resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) { chan_[0][static_cast<std::size_t>(i)] = inter[i]; }
            enc_.encode(chan_[0].data(), n, bps, &sub[0]);
        } else {
            for (std::size_t c = 0; c < 4; ++c) { chan_[c].resize(static_cast<std::size_t>(n)); }
            for (int i = 0; i < n; ++i) {
                const std::int32_t l = inter[2 * i], r = inter[2 * i + 1];
                const std::size_t k = static_cast<std::size_t>(i);
                chan_[0][k] = l; chan_[1][k] = r;
                chan_[2][k] = static_cast<std::int32_t>((static_cast<std::int64_t>(l) + r) >> 1);   // mid
                chan_[3][k] = l - r;                                                                  // side
            }
            const std::size_t sL = enc_.encode(chan_[0].data(), n, bps, nullptr), sR = enc_.encode(chan_[1].data(), n, bps, nullptr);
            const std::size_t sM = enc_.encode(chan_[2].data(), n, bps, nullptr), sS = enc_.encode(chan_[3].data(), n, bps + 1, nullptr);
            std::size_t best = sL + sR; assign = 1;
            if (sL + sS < best) { best = sL + sS; assign = 8; }
            if (sS + sR < best) { best = sS + sR; assign = 9; }
            if (sM + sS < best) { best = sM + sS; assign = 10; }
            const std::size_t a = assign == 1 ? 0 : (assign == 8 ? 0 : (assign == 9 ? 3 : 2));
            const std::size_t b = assign == 1 ? 1 : (assign == 8 ? 3 : (assign == 9 ? 1 : 3));
            enc_.encode(chan_[a].data(), n, bps + (a == 3 ? 1 : 0), &sub[0]);
            enc_.encode(chan_[b].data(), n, bps + (b == 3 ? 1 : 0), &sub[1]);
        }
        const int bpsCode = bps == 16 ? 4 : 6;
        h.push_back(static_cast<std::uint8_t>((assign << 4) | (bpsCode << 1)));
        detail::putUtf8Number(h, frameNumber);
        if (bsExtraBytes == 1) { h.push_back(static_cast<std::uint8_t>(n - 1)); }
        if (bsExtraBytes == 2) { h.push_back(static_cast<std::uint8_t>((n - 1) >> 8)); h.push_back(static_cast<std::uint8_t>(n - 1)); }
        if (srBytes == 1) { h.push_back(static_cast<std::uint8_t>(srExtra)); }
        if (srBytes == 2) { h.push_back(static_cast<std::uint8_t>(srExtra >> 8)); h.push_back(static_cast<std::uint8_t>(srExtra)); }
        h.push_back(detail::crc8(h.data(), h.size()));
        detail::BitWriter all;
        for (std::uint8_t b : h) { all.put(b, 8); }
        all.append(sub[0]);
        if (ch == 2) { all.append(sub[1]); }
        all.alignZero();
        out.assign(all.bytes().begin(), all.bytes().end());
        const std::uint16_t c = detail::crc16(out.data(), out.size());
        out.push_back(static_cast<std::uint8_t>(c >> 8)); out.push_back(static_cast<std::uint8_t>(c));
    }

private:
    struct FrameRec { std::uint64_t firstSample, offset; std::uint16_t samples; };

    static bool fail(std::string* err, const std::string& m) { if (err != nullptr) { *err = m; } return false; }
    static void put64(std::vector<std::uint8_t>& v, std::uint64_t x) { for (int i = 7; i >= 0; --i) { v.push_back(static_cast<std::uint8_t>(x >> (8 * i))); } }
    static void appendMetaHeader(std::vector<std::uint8_t>& v, int type, bool last, std::uint32_t len) {
        v.push_back(static_cast<std::uint8_t>((last ? 0x80 : 0) | type));
        v.push_back(static_cast<std::uint8_t>(len >> 16)); v.push_back(static_cast<std::uint8_t>(len >> 8)); v.push_back(static_cast<std::uint8_t>(len));
    }

    void fillStreamInfo(std::uint8_t* p, bool unknown) {
        // min = max = the block size, except a stream of one short block (the RFC excludes the last block from the minimum)
        const int blk = (!unknown && shortOnly_) ? std::max(16, lastBlock_) : opt_.blockSize;
        p[0] = static_cast<std::uint8_t>(blk >> 8); p[1] = static_cast<std::uint8_t>(blk);
        p[2] = static_cast<std::uint8_t>(blk >> 8); p[3] = static_cast<std::uint8_t>(blk);
        const std::uint32_t minF = unknown ? 0 : minFrame_, maxF = unknown ? 0 : maxFrame_;
        p[4] = static_cast<std::uint8_t>(minF >> 16); p[5] = static_cast<std::uint8_t>(minF >> 8); p[6] = static_cast<std::uint8_t>(minF);
        p[7] = static_cast<std::uint8_t>(maxF >> 16); p[8] = static_cast<std::uint8_t>(maxF >> 8); p[9] = static_cast<std::uint8_t>(maxF);
        const std::uint64_t total = unknown ? 0 : totalFrames_;
        // 20 bits rate, 3 bits channels-1, 5 bits bits-1, 36 bits total samples: 64 bits big-endian
        const std::uint64_t packed = (static_cast<std::uint64_t>(opt_.sampleRate) << 44) | (static_cast<std::uint64_t>(opt_.channels - 1) << 41) |
                                     (static_cast<std::uint64_t>(opt_.bitsPerSample - 1) << 36) | (total & 0xFFFFFFFFFull);
        for (int i = 0; i < 8; ++i) { p[10 + i] = static_cast<std::uint8_t>(packed >> (56 - 8 * i)); }
        if (unknown) { std::memset(p + 18, 0, 16); } else { Md5 m = md5_; const auto d = m.finish(); std::memcpy(p + 18, d.data(), 16); }
    }

    bool flushBlock() {
        const int ch = opt_.channels;
        const int n = static_cast<int>(pending_.size() / static_cast<std::size_t>(ch));
        const std::size_t bytesPer = static_cast<std::size_t>(opt_.bitsPerSample / 8);
        md5buf_.resize(pending_.size() * bytesPer);
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(pending_[i]);
            for (std::size_t b = 0; b < bytesPer; ++b) { md5buf_[i * bytesPer + b] = static_cast<std::uint8_t>(v >> (8 * b)); }
        }
        md5_.update(md5buf_.data(), md5buf_.size());

        frameBytes_.clear();
        encodeFrame(pending_.data(), n, frameNo_, frameBytes_);
        const std::uint64_t off = bytesWritten_ - firstFrameOffset_;
        file_.write(reinterpret_cast<const char*>(frameBytes_.data()), static_cast<std::streamsize>(frameBytes_.size()));
        file_.flush();
        if (!file_) { failed_ = true; error_ = "write failed (disk full or file removed)"; pending_.clear(); return false; }
        bytesWritten_ += frameBytes_.size();
        const std::uint32_t fs = static_cast<std::uint32_t>(frameBytes_.size());
        if (frameNo_ == 0) { minFrame_ = maxFrame_ = fs; } else { minFrame_ = std::min(minFrame_, fs); maxFrame_ = std::max(maxFrame_, fs); }
        index_.push_back({totalFrames_, off, static_cast<std::uint16_t>(n)});
        totalFrames_ += static_cast<std::uint64_t>(n);
        shortOnly_ = (frameNo_ == 0) && n != opt_.blockSize;
        lastBlock_ = n;
        ++frameNo_;
        pending_.clear();
        return true;
    }

    Options opt_;
    std::fstream file_;
    bool open_ = false, failed_ = false, shortOnly_ = false;
    int lastBlock_ = 0;
    std::string error_;
    std::vector<std::int32_t> pending_;
    std::vector<std::uint8_t> md5buf_, frameBytes_;
    std::vector<std::int32_t> chan_[4];
    detail::SubframeEncoder enc_;
    Md5 md5_;
    std::uint64_t frameNo_ = 0, totalFrames_ = 0, bytesWritten_ = 0, firstFrameOffset_ = 0;
    std::uint32_t minFrame_ = 0, maxFrame_ = 0;
    std::size_t streamInfoAt_ = 8, seekTableAt_ = 0;
    std::vector<FrameRec> index_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// AsyncRecorder: a wait-free producer side and one worker thread that converts, encodes and writes.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class AsyncRecorder {
public:
    ~AsyncRecorder() { stop(nullptr); }

    // `ringFrames` is the capacity of the hand-over ring in frames (default about 2.7 s at 48 kHz).
    bool start(const std::string& path, const Options& opt, std::string* err, std::size_t ringFrames = 131072) {
        if (running_.load()) { if (err != nullptr) { *err = "already running"; } return false; }
        if (!writer_.open(path, opt, err)) { return false; }
        channels_ = static_cast<std::size_t>(opt.channels);
        bits_ = opt.bitsPerSample;
        cap_ = 1; while (cap_ < ringFrames * channels_) { cap_ <<= 1; }
        ring_.assign(cap_, 0.0f);
        head_.store(0); tail_.store(0); dropped_.store(0); accepted_.store(0); workerFailed_.store(false);
        lastError_.clear();
        stopFlag_.store(false);
        running_.store(true);
        worker_ = std::thread([this] { run(); });
        return true;
    }

    // Producer thread only. Wait-free and allocation-free. Returns the frames accepted; the rest are counted as dropped.
    std::size_t push(const float* interleaved, std::size_t frames) {
        if (!running_.load(std::memory_order_relaxed)) { return 0; }
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        const std::size_t freeSamples = cap_ - static_cast<std::size_t>(h - t);
        const std::size_t fit = std::min(frames, freeSamples / channels_);
        for (std::size_t i = 0; i < fit * channels_; ++i) { ring_[static_cast<std::size_t>((h + i) & (cap_ - 1))] = interleaved[i]; }
        head_.store(h + fit * channels_, std::memory_order_release);
        accepted_.fetch_add(fit, std::memory_order_relaxed);
        if (fit < frames) { dropped_.fetch_add(frames - fit, std::memory_order_relaxed); }
        return fit;
    }

    // Drains what was accepted, finishes the file, joins the worker. Control thread. Idempotent.
    bool stop(std::string* err = nullptr) {
        if (!running_.load()) { return true; }
        stopFlag_.store(true);
        if (worker_.joinable()) { worker_.join(); }
        running_.store(false);
        std::string e;
        const bool fin = writer_.finish(&e);
        const bool ok = fin && !workerFailed_.load();
        if (!ok && err != nullptr) { *err = lastError_.empty() ? e : lastError_; }
        return ok;
    }

    bool running() const { return running_.load(); }
    std::uint64_t droppedFrames() const { return dropped_.load(); }
    std::uint64_t acceptedFrames() const { return accepted_.load(); }
    bool failed() const { return workerFailed_.load(); }
    std::uint64_t framesWrittenAfterStop() const { return writer_.framesWritten(); }   // meaningful after stop() only

private:
    void run() {
        const std::size_t chunkFrames = 2048;
        std::vector<std::int32_t> q(chunkFrames * channels_);
        const double scale = static_cast<double>(1u << (bits_ - 1));
        const std::int64_t hi = (1ll << (bits_ - 1)) - 1, lo = -(1ll << (bits_ - 1));
        for (;;) {
            const bool stopping = stopFlag_.load();
            const std::uint64_t h = head_.load(std::memory_order_acquire);
            std::uint64_t t = tail_.load(std::memory_order_relaxed);
            bool did = false;
            while (h - t >= channels_) {
                const std::size_t frames = std::min<std::size_t>(chunkFrames, static_cast<std::size_t>(h - t) / channels_);
                for (std::size_t i = 0; i < frames * channels_; ++i) {
                    const float v = ring_[static_cast<std::size_t>((t + i) & (cap_ - 1))];
                    const double s = std::isfinite(v) ? static_cast<double>(v) * scale : 0.0;
                    q[i] = static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(std::llround(s)), lo, hi));
                }
                t += frames * channels_;
                tail_.store(t, std::memory_order_release);
                std::string e;
                if (!workerFailed_.load() && !writer_.write(q.data(), frames, &e)) { lastError_ = e; workerFailed_.store(true); }
                did = true;
            }
            if (stopping && !did) { return; }
            if (!did) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        }
    }

    Writer writer_;
    std::thread worker_;
    std::vector<float> ring_;
    std::size_t cap_ = 0, channels_ = 1;
    int bits_ = 16;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0}, accepted_{0};
    std::atomic<bool> running_{false}, stopFlag_{false}, workerFailed_{false};
    std::string lastError_;
};

}  // namespace cascade::core::flac
