// Reads the centre-frequency metadata that some SDR programs write into the header of an I/Q WAV file (the RIFF `auxi` chunk), and lists every chunk.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md, row P16). The existing IqFileSource reads rate and channel format only and the file
// reader of feat/signal-blocks-2 (`iq_readers.hpp`, not in this base) guesses a centre from the FILE NAME; this is the header route, and the join is:
// IqFileSource::open() calls readWavMeta() and, when centerKnown, uses meta.centerHz instead of the nominal 0 Hz.
//
// THE `auxi` CHUNK (layout verified, see the record): chunk id "auxi", little-endian, payload
//     offset  0  SYSTEMTIME StartTime   (eight 16-bit words: year, month, day-of-week, day, hour, minute, second, millisecond)
//     offset 16  SYSTEMTIME StopTime    (same)
//     offset 32  uint32 CenterFreq      (Hz)
//     offset 36  uint32 ADFrequency     (Hz; what it means differs between writers: a constant ADC clock in one, the sample rate in another)
//     offset 40  uint32 IFFrequency     (Hz)
//     offset 44  uint32 Bandwidth       (Hz)
//     offset 48  uint32 IQOffset
//     offset 52 .. further fields and a file-name field up to the chunk size (164 bytes in the files read)
// Sources: the layout of the first seven fields from the BSD-licensed CuteSDR wave-file writer (as published in Debian's source tree) and the byte layout
// of three real files on this PC that carry the chunk (all three: size 164, CenterFreq at offset 32 equal to the band named in the recording's folder).
// Only CenterFreq and the two times are used; the other fields are reported, never trusted. A chunk shorter than 36 bytes yields no centre.
//
// BOUNDS: at most 256 chunk headers are walked; a chunk is read only for its first 256 bytes; sizes are checked against the file size before use; a data
// size of 0 or 0xFFFFFFFF (a streaming writer's placeholder) ends the walk (RF64 `ds64` is honoured for the data size). Nothing allocates beyond a few
// hundred bytes. An unreadable or odd file returns ok = false with a reason, never an exception.
//
// A SECOND, UNDOCUMENTED CANDIDATE: a `FREQ` chunk (seen in one real file, written by a SignalHound BB60C program: the chunk text says so) whose first two
// little-endian 64-bit integers read as 131726151 and 135498, the second equal to that file's sample rate. When the second integer equals the fmt chunk's
// sample rate the first is offered as `freqChunkHz` - a CANDIDATE only (centerKnown stays false): the layout is documented in nothing opened and one
// file is not a specification. The owner decides whether the file source may use it.
//
// NOT PARSED: Spectrum Lab's `inf1` text chunk (documented as `token=value` text with `rf=` but the separator and the unit of rf are not stated on the
// page read) and SDR Console's `rcvr` chunk (no description found): their ids are listed in `chunks` so a later reader can look at real files.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace cascade::source::wavmeta {

constexpr std::size_t kMaxChunks = 256;
constexpr std::size_t kMaxChunkRead = 256;

struct SystemTime {
    bool valid = false;                  // every field in range
    int year = 0, month = 0, dayOfWeek = 0, day = 0, hour = 0, minute = 0, second = 0, millisecond = 0;
    long long unixMs = 0;                // UTC, only when valid
};

struct AuxiInfo {
    bool present = false;
    std::uint32_t chunkSize = 0;
    SystemTime start, stop;
    bool hasCenter = false;              // the chunk was long enough for CenterFreq and it is not 0
    std::uint32_t centerHz = 0;
    std::uint32_t adFrequencyHz = 0;
    std::uint32_t ifFrequencyHz = 0;
    std::uint32_t bandwidthHz = 0;
    std::uint32_t iqOffset = 0;
};

struct ChunkInfo {
    char id[5] = {0, 0, 0, 0, 0};
    std::uint64_t offset = 0;            // of the chunk header
    std::uint64_t size = 0;
};

struct WavMeta {
    bool ok = false;
    std::string error;
    std::string container;               // "RIFF", "RF64" or "BW64"
    unsigned formatTag = 0, channels = 0, bitsPerSample = 0;
    std::uint32_t sampleRateHz = 0;
    std::uint64_t dataOffset = 0, dataBytes = 0;
    bool hasData = false;
    std::vector<ChunkInfo> chunks;       // in file order, at most kMaxChunks
    AuxiInfo auxi;
    bool centerKnown = false;
    double centerHz = 0.0;
    std::string centerSource;            // the words the screen shows: where the centre came from
    bool startKnown = false;
    long long startUnixMs = 0;
    bool hasFreqChunkCandidate = false;  // see the header comment: a candidate, never used for centerKnown
    double freqChunkHz = 0.0;
};

// Random-access bytes: a file, or memory in a test.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const = 0;
    virtual std::size_t readAt(std::uint64_t offset, unsigned char* dst, std::size_t n) = 0;   // fewer only at the end
};

class MemorySource final : public ByteSource {
public:
    MemorySource(const unsigned char* d, std::size_t n) : d_(d), n_(n) {}
    std::uint64_t size() const override { return n_; }
    std::size_t readAt(std::uint64_t off, unsigned char* dst, std::size_t n) override {
        if (off >= n_) { return 0; }
        const std::size_t k = std::min<std::size_t>(n, n_ - static_cast<std::size_t>(off));
        std::memcpy(dst, d_ + off, k);
        return k;
    }
private:
    const unsigned char* d_;
    std::size_t n_;
};

class FileSource final : public ByteSource {
public:
    explicit FileSource(const std::filesystem::path& p) {
#if defined(_WIN32)
        f_ = _wfopen(p.c_str(), L"rb");
#else
        f_ = std::fopen(p.c_str(), "rb");
#endif
        if (f_ != nullptr) {
#if defined(_WIN32)
            _fseeki64(f_, 0, SEEK_END); size_ = static_cast<std::uint64_t>(_ftelli64(f_));
#else
            fseeko(f_, 0, SEEK_END); size_ = static_cast<std::uint64_t>(ftello(f_));
#endif
        }
    }
    ~FileSource() override { if (f_ != nullptr) { std::fclose(f_); } }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    bool open() const { return f_ != nullptr; }
    std::uint64_t size() const override { return size_; }
    std::size_t readAt(std::uint64_t off, unsigned char* dst, std::size_t n) override {
        if (f_ == nullptr || off >= size_) { return 0; }
#if defined(_WIN32)
        if (_fseeki64(f_, static_cast<long long>(off), SEEK_SET) != 0) { return 0; }
#else
        if (fseeko(f_, static_cast<off_t>(off), SEEK_SET) != 0) { return 0; }
#endif
        return std::fread(dst, 1, n, f_);
    }
private:
    FILE* f_ = nullptr;
    std::uint64_t size_ = 0;
};

namespace detail {

inline std::uint32_t u32(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
inline std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint64_t u64(const unsigned char* p) { return static_cast<std::uint64_t>(u32(p)) | (static_cast<std::uint64_t>(u32(p + 4)) << 32); }

inline long long floorDiv(long long a, long long b) { long long q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) { --q; } return q; }

// days from civil (Howard Hinnant's published algorithm, public domain)
inline long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = floorDiv(y, 400);
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

inline bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline SystemTime systemTime(const unsigned char* p) {
    SystemTime t;
    t.year = u16(p); t.month = u16(p + 2); t.dayOfWeek = u16(p + 4); t.day = u16(p + 6);
    t.hour = u16(p + 8); t.minute = u16(p + 10); t.second = u16(p + 12); t.millisecond = u16(p + 14);
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (t.year < 1970 || t.year > 2200 || t.month < 1 || t.month > 12) { return t; }
    const int maxDay = dim[t.month - 1] + ((t.month == 2 && leap(t.year)) ? 1 : 0);
    if (t.day < 1 || t.day > maxDay || t.hour > 23 || t.minute > 59 || t.second > 59 || t.millisecond > 999 || t.dayOfWeek > 6) { return t; }
    t.valid = true;
    t.unixMs = (daysFromCivil(t.year, static_cast<unsigned>(t.month), static_cast<unsigned>(t.day)) * 86400LL + t.hour * 3600LL + t.minute * 60LL + t.second) * 1000LL + t.millisecond;
    return t;
}

inline AuxiInfo parseAuxi(const unsigned char* p, std::size_t have, std::uint32_t chunkSize) {
    AuxiInfo a;
    a.present = true;
    a.chunkSize = chunkSize;
    if (have >= 16) { a.start = systemTime(p); }
    if (have >= 32) { a.stop = systemTime(p + 16); }
    if (have >= 36) { a.centerHz = u32(p + 32); a.hasCenter = a.centerHz != 0; }
    if (have >= 40) { a.adFrequencyHz = u32(p + 36); }
    if (have >= 44) { a.ifFrequencyHz = u32(p + 40); }
    if (have >= 48) { a.bandwidthHz = u32(p + 44); }
    if (have >= 52) { a.iqOffset = u32(p + 48); }
    return a;
}

}  // namespace detail

// Walks the chunks of the RIFF / RF64 file behind `src`.
inline WavMeta readWavMeta(ByteSource& src) {
    WavMeta m;
    auto fail = [&](const char* msg) { m.ok = false; m.error = msg; return m; };
    const std::uint64_t size = src.size();
    unsigned char head[12];
    if (size < 12 || src.readAt(0, head, 12) != 12) { return fail("the file is shorter than a WAV header"); }
    if (std::memcmp(head, "RIFF", 4) == 0) { m.container = "RIFF"; }
    else if (std::memcmp(head, "RF64", 4) == 0) { m.container = "RF64"; }
    else if (std::memcmp(head, "BW64", 4) == 0) { m.container = "BW64"; }
    else { return fail("not a RIFF / RF64 file"); }
    if (std::memcmp(head + 8, "WAVE", 4) != 0) { return fail("not a WAVE file"); }
    std::uint64_t pos = 12;
    std::uint64_t ds64Data = 0;
    bool haveDs64 = false, haveFmt = false, haveFreqChunk = false;
    std::uint64_t freqFirst = 0, freqSecond = 0;
    for (std::size_t n = 0; n < kMaxChunks && pos + 8 <= size; ++n) {
        unsigned char ch[8];
        if (src.readAt(pos, ch, 8) != 8) { break; }
        ChunkInfo ci;
        std::memcpy(ci.id, ch, 4);
        ci.id[4] = '\0';
        for (int i = 0; i < 4; ++i) { if (static_cast<unsigned char>(ci.id[i]) < 0x20 || static_cast<unsigned char>(ci.id[i]) > 0x7E) { ci.id[i] = '?'; } }
        ci.offset = pos;
        std::uint64_t csize = detail::u32(ch + 4);
        const std::uint64_t body = pos + 8;
        const bool isData = std::memcmp(ch, "data", 4) == 0;
        if (isData && csize == 0xFFFFFFFFull && haveDs64) { csize = ds64Data; }
        ci.size = csize;
        m.chunks.push_back(ci);
        unsigned char buf[kMaxChunkRead];
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(csize, kMaxChunkRead));
        const std::size_t avail = static_cast<std::size_t>(std::min<std::uint64_t>(want, size > body ? size - body : 0));
        if (std::memcmp(ch, "fmt ", 4) == 0 && !haveFmt) {
            if (avail >= 16 && src.readAt(body, buf, 16) == 16) {
                haveFmt = true;
                m.formatTag = detail::u16(buf); m.channels = detail::u16(buf + 2); m.sampleRateHz = detail::u32(buf + 4); m.bitsPerSample = detail::u16(buf + 14);
            }
        } else if (std::memcmp(ch, "ds64", 4) == 0 && avail >= 16 && src.readAt(body, buf, 16) == 16) {
            ds64Data = detail::u64(buf + 8);
            haveDs64 = true;
        } else if (std::memcmp(ch, "auxi", 4) == 0 && !m.auxi.present) {
            const std::size_t got = avail > 0 ? src.readAt(body, buf, avail) : 0;
            m.auxi = detail::parseAuxi(buf, got, static_cast<std::uint32_t>(std::min<std::uint64_t>(csize, 0xFFFFFFFFull)));
        } else if (std::memcmp(ch, "FREQ", 4) == 0 && !haveFreqChunk && avail >= 16 && src.readAt(body, buf, 16) == 16) {
            haveFreqChunk = true;
            freqFirst = detail::u64(buf); freqSecond = detail::u64(buf + 8);
        } else if (isData && !m.hasData) {
            m.hasData = true; m.dataOffset = body; m.dataBytes = csize;
        }
        if (isData && (csize == 0 || csize == 0xFFFFFFFFull)) { break; }       // a streaming writer's placeholder: nothing reliable follows
        if (csize > size || body + csize > size) { break; }                    // runs past the end: this was the last chunk we can trust
        pos = body + csize + (csize & 1);
    }
    if (m.chunks.empty()) { return fail("no chunks"); }
    m.ok = true;
    if (m.auxi.present && m.auxi.hasCenter) {
        m.centerKnown = true;
        m.centerHz = static_cast<double>(m.auxi.centerHz);
        m.centerSource = "the file's auxi chunk (CenterFreq)";
    }
    if (haveFreqChunk && haveFmt && m.sampleRateHz != 0 && freqSecond == m.sampleRateHz && freqFirst > 0 && freqFirst <= 1000000000000ull) {
        m.hasFreqChunkCandidate = true;
        m.freqChunkHz = static_cast<double>(freqFirst);
    }
    if (m.auxi.present && m.auxi.start.valid) { m.startKnown = true; m.startUnixMs = m.auxi.start.unixMs; }
    return m;
}

inline WavMeta readWavMeta(const std::filesystem::path& path) {
    FileSource f(path);
    if (!f.open()) { WavMeta m; m.error = "cannot open the file"; return m; }
    return readWavMeta(f);
}

inline WavMeta readWavMeta(const unsigned char* data, std::size_t n) {
    MemorySource s(data, n);
    return readWavMeta(s);
}

}  // namespace cascade::source::wavmeta
