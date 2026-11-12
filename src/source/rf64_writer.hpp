// RF64 / BW64-capable WAV writer for I/Q recordings: a plain RIFF/WAVE file that is promoted to RF64 when the data
// would pass 4 GiB, with a header that is brought up to date every few tens of megabytes so a crash loses a bounded tail
// and repairWavHeader() can recover the rest.
//
// STANDALONE LAYER. Nothing here is wired: the recorder (core/recorder.*) still writes its own RIFF files. The wiring
// hand-off is in docs/feature-records/core-formats.md. Round-trips through source/iq_readers.hpp (openWav).
//
// SOURCES (clean room: specifications only)
//  * EBU Tech 3306 v1.0 (2007), https://tech.ebu.ch/docs/tech/tech3306v1_0.pdf, as read for docs/feature-records/signal-blocks-2.md
//    (2026-11-11): ID "RF64" instead of "RIFF", riff size field 0xFFFFFFFF, a "ds64" chunk FIRST after "WAVE" with 64-bit
//    riffSize, dataSize, sampleCount and a 32-bit table length; the 32-bit size of the data chunk is 0xFFFFFFFF; a
//    "JUNK" chunk of the same size can be reserved at the start so the file is promoted in place without moving the data
//    (the document describes this use of JUNK). The same layout is used with the ID "BW64" in ITU-R BS.2088 (UNVERIFIED:
//    BS.2088 not read; this writer writes "RF64" and the reader accepts both).
//  * WAVEFORMATEX / WAVEFORMATEXTENSIBLE: Microsoft documentation (see iq_readers.hpp). 24- and 32-bit PCM are written as
//    WAVE_FORMAT_EXTENSIBLE with SubFormat PCM (valid bits = container bits, channel mask front-left | front-right).
//
// LAYOUT (every file): "RIFF"|"RF64", size, "WAVE", "JUNK"|"ds64" (28-byte body), "fmt ", "data", samples. Header size is
// fixed at open, so promotion rewrites only the header bytes.
//
// THREADS: one writer object, one thread. (The recorder's worker owns it.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cascade::source::iqfile {

enum class WriteFormat : int { S16, S24, S32, F32 };

struct Rf64Options {
    double sampleRateHz = 0.0;                         // required, a whole number 1 .. 4294967295
    WriteFormat format = WriteFormat::F32;
    std::uint64_t headerUpdateBytes = 32ull << 20;     // brought up to date after every this many data bytes (0 = only on close / flush)
    bool forceRf64 = false;                            // write RF64 from the start (test and tools)
    // The largest RIFF size a plain file may reach before it is promoted. The default is the format's own limit; a test lowers
    // it to prove promotion without writing 4 GiB.
    std::uint64_t plainLimitBytes = 0xFFFFFFFEull;
};

namespace wavdetail {

constexpr std::uint32_t kUnknownSize = 0xFFFFFFFFu;
constexpr std::uint64_t kDs64Body = 28;

inline void put16(unsigned char* p, std::uint16_t v) { p[0] = static_cast<unsigned char>(v); p[1] = static_cast<unsigned char>(v >> 8); }
inline void put32(unsigned char* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) { p[i] = static_cast<unsigned char>(v >> (8 * i)); } }
inline void put64(unsigned char* p, std::uint64_t v) { for (int i = 0; i < 8; ++i) { p[i] = static_cast<unsigned char>(v >> (8 * i)); } }
inline std::uint32_t get32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint16_t get16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

inline std::size_t bytesPerComponent(WriteFormat f) {
    switch (f) { case WriteFormat::S16: return 2; case WriteFormat::S24: return 3; default: return 4; }
}
inline bool extensible(WriteFormat f) { return f == WriteFormat::S24 || f == WriteFormat::S32; }

// The fixed part of the header: everything before the data bytes, with sizes zero (patch() fills them).
inline std::vector<unsigned char> buildHeader(const Rf64Options& o) {
    const bool ext = extensible(o.format);
    const std::size_t fmtBody = ext ? 40 : 16;
    std::vector<unsigned char> h(12 + 8 + kDs64Body + 8 + fmtBody + 8, 0);
    unsigned char* p = h.data();
    std::memcpy(p, "RIFF", 4);
    std::memcpy(p + 8, "WAVE", 4);
    std::memcpy(p + 12, "JUNK", 4);
    put32(p + 16, static_cast<std::uint32_t>(kDs64Body));
    unsigned char* f = p + 12 + 8 + kDs64Body;
    std::memcpy(f, "fmt ", 4);
    put32(f + 4, static_cast<std::uint32_t>(fmtBody));
    unsigned char* b = f + 8;
    const std::uint16_t bits = static_cast<std::uint16_t>(8 * bytesPerComponent(o.format));
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(2 * bytesPerComponent(o.format));
    const std::uint32_t rate = static_cast<std::uint32_t>(o.sampleRateHz);
    put16(b, ext ? 0xFFFE : (o.format == WriteFormat::F32 ? 3 : 1));
    put16(b + 2, 2);
    put32(b + 4, rate);
    put32(b + 8, rate * blockAlign);
    put16(b + 12, blockAlign);
    put16(b + 14, bits);
    if (ext) {
        put16(b + 16, 22);
        put16(b + 18, bits);          // valid bits
        put32(b + 20, 0x3);           // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
        static const unsigned char guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        std::memcpy(b + 24, guid, 16);   // PCM
    }
    unsigned char* d = f + 8 + fmtBody;
    std::memcpy(d, "data", 4);
    return h;
}

// Writes the size fields for `dataBytes` of data (a whole number of frames) into an open stream and puts the write position
// at the end of the data. `rf64` selects the layout. headerBytes = offset of the first data byte.
inline bool patch(std::fstream& f, std::uint64_t headerBytes, std::uint64_t dataBytes, std::uint64_t frames, bool rf64) {
    const std::uint64_t riffSize = headerBytes + dataBytes - 8;
    unsigned char a[40];
    f.clear();
    if (rf64) {
        std::memcpy(a, "RF64", 4);
        put32(a + 4, kUnknownSize);
        f.seekp(0);
        f.write(reinterpret_cast<const char*>(a), 8);
        std::memcpy(a, "ds64", 4);
        put32(a + 4, static_cast<std::uint32_t>(kDs64Body));
        put64(a + 8, riffSize);
        put64(a + 16, dataBytes);
        put64(a + 24, frames);
        put32(a + 32, 0);
        f.seekp(12);
        f.write(reinterpret_cast<const char*>(a), 36);
        put32(a, kUnknownSize);
        f.seekp(static_cast<std::streamoff>(headerBytes - 4));
        f.write(reinterpret_cast<const char*>(a), 4);
    } else {
        std::memcpy(a, "RIFF", 4);
        put32(a + 4, static_cast<std::uint32_t>(riffSize));
        f.seekp(0);
        f.write(reinterpret_cast<const char*>(a), 8);
        std::memcpy(a, "JUNK", 4);
        put32(a + 4, static_cast<std::uint32_t>(kDs64Body));
        std::memset(a + 8, 0, 28);
        f.seekp(12);
        f.write(reinterpret_cast<const char*>(a), 36);
        put32(a, static_cast<std::uint32_t>(dataBytes));
        f.seekp(static_cast<std::streamoff>(headerBytes - 4));
        f.write(reinterpret_cast<const char*>(a), 4);
    }
    f.flush();
    f.seekp(static_cast<std::streamoff>(headerBytes + dataBytes));
    return static_cast<bool>(f);
}

}  // namespace wavdetail

class Rf64Writer {
public:
    Rf64Writer() = default;
    Rf64Writer(const Rf64Writer&) = delete;
    Rf64Writer& operator=(const Rf64Writer&) = delete;
    ~Rf64Writer() { std::string ignore; close(&ignore); }

    bool open(const std::string& path, const Rf64Options& o, std::string* err) {
        std::string ignore;
        close(&ignore);
        if (!(o.sampleRateHz >= 1.0) || o.sampleRateHz > 4294967295.0 || std::floor(o.sampleRateHz) != o.sampleRateHz) {
            return fail(err, "sample rate must be a whole number of hertz from 1 to 4294967295");
        }
        if (o.plainLimitBytes < 4096) { return fail(err, "plain size limit too small"); }
        opt_ = o;
        header_ = wavdetail::buildHeader(o);
        comp_ = wavdetail::bytesPerComponent(o.format);
        frameBytes_ = 2 * comp_;
        f_.open(std::filesystem::path(path), std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f_) { return fail(err, "cannot create " + path); }
        path_ = path;
        f_.write(reinterpret_cast<const char*>(header_.data()), static_cast<std::streamsize>(header_.size()));
        rf64_ = o.forceRf64;
        dataBytes_ = 0;
        frames_ = 0;
        sinceUpdate_ = 0;
        failed_ = false;
        if (!wavdetail::patch(f_, header_.size(), 0, 0, rf64_)) { f_.close(); return fail(err, "cannot write the header of " + path); }
        return true;
    }

    bool isOpen() const { return f_.is_open(); }
    bool isRf64() const { return rf64_; }
    std::uint64_t framesWritten() const { return frames_; }
    std::uint64_t dataBytes() const { return dataBytes_; }
    std::uint64_t headerBytes() const { return header_.size(); }

    // Appends `n` complex samples. Non-finite values are written as 0, values beyond full scale are clipped. False on a disk
    // error (the writer then refuses everything and close() reports it), or if not open.
    bool write(const std::complex<float>* iq, std::size_t n, std::string* err = nullptr) {
        if (!f_.is_open()) { return fail(err, "writer is not open"); }
        if (failed_) { return fail(err, "an earlier write failed"); }
        constexpr std::size_t kBlock = 16384;
        std::vector<unsigned char>& buf = buf_;
        buf.resize(kBlock * frameBytes_);
        std::size_t done = 0;
        while (done < n) {
            const std::size_t m = std::min(kBlock, n - done);
            encode(iq + done, m, buf.data());
            const std::uint64_t add = static_cast<std::uint64_t>(m) * frameBytes_;
            if (!rf64_ && header_.size() + dataBytes_ + add - 8 > opt_.plainLimitBytes) {
                if (!wavdetail::patch(f_, header_.size(), dataBytes_, frames_, true)) { failed_ = true; return fail(err, "disk error while promoting to RF64"); }
                rf64_ = true;
            }
            f_.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(add));
            if (!f_) { failed_ = true; return fail(err, "disk error while writing " + path_); }
            dataBytes_ += add;
            frames_ += m;
            sinceUpdate_ += add;
            done += m;
            if (opt_.headerUpdateBytes != 0 && sinceUpdate_ >= opt_.headerUpdateBytes) {
                if (!flushHeader(err)) { return false; }
            }
        }
        return true;
    }

    // Brings the header up to date and flushes: after this the file is a valid recording of everything written so far.
    bool flushHeader(std::string* err = nullptr) {
        if (!f_.is_open()) { return fail(err, "writer is not open"); }
        if (!wavdetail::patch(f_, header_.size(), dataBytes_, frames_, rf64_)) { failed_ = true; return fail(err, "disk error while updating the header"); }
        sinceUpdate_ = 0;
        return true;
    }

    bool close(std::string* err = nullptr) {
        if (!f_.is_open()) { return true; }
        bool ok = true;
        if (!failed_) { ok = flushHeader(err); } else { fail(err, "a write failed: the file is incomplete (repairWavHeader can recover it)"); ok = false; }
        f_.close();
        return ok && !failed_;
    }

    // TEST HOOK: drops the file handle with the header exactly as the last update left it, as a crash would.
    void abandonForTest() { f_.close(); }

private:
    static bool fail(std::string* err, const std::string& m) { if (err != nullptr) { *err = m; } return false; }

    void encode(const std::complex<float>* in, std::size_t n, unsigned char* out) const {
        const float* x = reinterpret_cast<const float*>(in);
        const std::size_t comps = n * 2;
        for (std::size_t i = 0; i < comps; ++i) {
            const float v = std::isfinite(x[i]) ? x[i] : 0.0f;
            switch (opt_.format) {
                case WriteFormat::S16: {
                    const long q = std::lround(std::clamp(static_cast<double>(v), -1.0, 1.0) * 32767.0);
                    wavdetail::put16(out + 2 * i, static_cast<std::uint16_t>(static_cast<std::int16_t>(q)));
                    break;
                }
                case WriteFormat::S24: {
                    const long q = std::lround(std::clamp(static_cast<double>(v), -1.0, 1.0) * 8388607.0);
                    const std::uint32_t u = static_cast<std::uint32_t>(static_cast<std::int32_t>(q));
                    out[3 * i] = static_cast<unsigned char>(u);
                    out[3 * i + 1] = static_cast<unsigned char>(u >> 8);
                    out[3 * i + 2] = static_cast<unsigned char>(u >> 16);
                    break;
                }
                case WriteFormat::S32: {
                    const long long q = std::llround(std::clamp(static_cast<double>(v), -1.0, 1.0) * 2147483647.0);
                    wavdetail::put32(out + 4 * i, static_cast<std::uint32_t>(static_cast<std::int32_t>(q)));
                    break;
                }
                case WriteFormat::F32: {
                    std::uint32_t u;
                    std::memcpy(&u, &v, 4);
                    wavdetail::put32(out + 4 * i, u);
                    break;
                }
            }
        }
    }

    Rf64Options opt_;
    std::vector<unsigned char> header_, buf_;
    std::fstream f_;
    std::string path_;
    std::size_t comp_ = 4, frameBytes_ = 8;
    std::uint64_t dataBytes_ = 0, frames_ = 0, sinceUpdate_ = 0;
    bool rf64_ = false, failed_ = false;
};

// Recovers a file this writer started but did not close (a crash, a power cut, a full disk): the sizes are recomputed from the
// file length, a partial last frame is left out of the data size, and the file is promoted to RF64 if it needs it. Only files
// with this writer's layout (reserved 28-byte JUNK or ds64 first) are accepted; anything else is refused and left untouched.
inline bool repairWavHeader(const std::string& path, std::string* err, std::uint64_t* framesOut = nullptr) {
    auto fail = [&](const std::string& m) { if (err != nullptr) { *err = m; } return false; };
    std::error_code ec;
    const std::uint64_t fsize = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) { return fail("cannot read the size of " + path); }
    std::fstream f(std::filesystem::path(path), std::ios::in | std::ios::out | std::ios::binary);
    if (!f) { return fail("cannot open " + path); }
    unsigned char h[160];
    f.read(reinterpret_cast<char*>(h), sizeof h);
    const std::size_t got = static_cast<std::size_t>(f.gcount());
    if (got < 12 + 36 + 8 + 16 + 8) { return fail("too short to be a recording from this writer"); }
    const bool rf = std::memcmp(h, "RF64", 4) == 0;
    if ((!rf && std::memcmp(h, "RIFF", 4) != 0) || std::memcmp(h + 8, "WAVE", 4) != 0) { return fail("not a RIFF/RF64 WAVE file"); }
    if ((std::memcmp(h + 12, "JUNK", 4) != 0 && std::memcmp(h + 12, "ds64", 4) != 0) || wavdetail::get32(h + 16) != wavdetail::kDs64Body) {
        return fail("no reserved 28-byte JUNK/ds64 chunk at the start: not written by this writer");
    }
    const std::size_t fo = 12 + 8 + wavdetail::kDs64Body;
    if (std::memcmp(h + fo, "fmt ", 4) != 0) { return fail("fmt chunk not where this writer puts it"); }
    const std::uint32_t fmtSize = wavdetail::get32(h + fo + 4);
    if (fmtSize != 16 && fmtSize != 40) { return fail("unexpected fmt chunk size"); }
    const std::size_t dh = fo + 8 + fmtSize;
    if (dh + 8 > got || std::memcmp(h + dh, "data", 4) != 0) { return fail("data chunk not where this writer puts it"); }
    const std::uint16_t blockAlign = wavdetail::get16(h + fo + 8 + 12);
    if (blockAlign == 0) { return fail("block align is zero"); }
    const std::uint64_t headerBytes = dh + 8;
    if (fsize < headerBytes) { return fail("file ends inside the header"); }
    std::uint64_t dataBytes = fsize - headerBytes;
    dataBytes -= dataBytes % blockAlign;
    const std::uint64_t frames = dataBytes / blockAlign;
    const bool needRf64 = rf || headerBytes + dataBytes - 8 > 0xFFFFFFFEull;
    if (!wavdetail::patch(f, headerBytes, dataBytes, frames, needRf64)) { return fail("cannot write the repaired header"); }
    if (framesOut != nullptr) { *framesOut = frames; }
    return true;
}

}  // namespace cascade::source::iqfile
