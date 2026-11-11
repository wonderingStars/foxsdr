// I/Q file readers behind one interface: raw .cu8 / .cs16 / .cf32, WAV (PCM 16/24/32, IEEE float 32,
// WAVE_FORMAT_EXTENSIBLE, RIFF and RF64/BW64 including files over 4 GB) and SigMF (.sigmf-meta + .sigmf-data).
//
// STANDALONE LAYER. Nothing here is wired: the existing playback source (source/iq_file_source.*, a looping
// 2-channel RIFF/WAVE reader for PCM16 and float32 only, with no RF64, no 24/32-bit PCM, no extensible, no raw
// and no SigMF) is untouched. This layer is what it (or a new source) could call:
//     std::string err;
//     auto r = iqfile::openAuto(path, params, &err);          // by extension; raw needs params
//     r->info().sampleRateHz, r->info().totalSamples ...
//     r->seek(n);  r->read(dst, count);                      // complex<float>, no looping, 0 at the end
//
// SOURCES (clean room: specifications only; read status in docs/feature-records/signal-blocks-2.md)
//  * RIFF/WAVE chunk layout, PCM and IEEE-float format tags: Microsoft WAVEFORMATEX / WAVEFORMATEXTENSIBLE
//    documentation (learn.microsoft.com, ksmedia.h WAVEFORMATEXTENSIBLE page, read 2026-11-11): layout
//    (Format, wValidBitsPerSample, dwChannelMask, SubFormat), the channel-mask bit table, "valid bits are left-
//    aligned in the container". The SubFormat GUID of PCM is {00000001-0000-0010-8000-00AA00389B71} and of IEEE
//    float {00000003-...}: Data1 is the 16-bit format tag; the common tail was seen on the same site's
//    subformat-GUID table; the two exact values are otherwise from memory (UNVERIFIED as printed values).
//  * RF64: EBU Tech 3306 v1.0 (2007), https://tech.ebu.ch/docs/tech/tech3306v1_0.pdf, read 2026-11-11: ID "RF64"
//    instead of "RIFF"; a mandatory "ds64" chunk FIRST after the RF64 header with 64-bit riffSize, dataSize,
//    sampleCount (then a 32-bit table length and the table); a 32-bit size field equal to 0xFFFFFFFF means "use
//    the ds64 value". BW64 (ITU-R BS.2088) uses the same layout with the ID "BW64" (UNVERIFIED: BS.2088 not read).
//  * SigMF: specification v1.2.6 as published at https://sigmf.org (read 2026-11-11): files .sigmf-meta (JSON) and
//    .sigmf-data; core:datatype = (r|c) + (f32|f64|i32|i16|u32|u16) + (_le|_be), or i8 / u8 alone; core:sample_rate,
//    core:version; captures[] with core:sample_start (required), core:frequency, core:datetime; annotations[] with
//    core:sample_start, core:sample_count, core:freq_lower_edge / core:freq_upper_edge, core:label.
//  * Raw 8-bit unsigned: the (u - 127.5) / 127.5 mapping the common RTL-SDR tools use (a convention, not a
//    standard; UNVERIFIED which tool does exactly this).
//
// WHAT IS SUPPORTED AND WHAT IS REFUSED (with a clear message)
//  Raw: cu8, cs16 (little-endian), cf32 (little-endian). WAV: PCM 16/24/32 bit and IEEE float 32 bit, two
//  channels (I, Q), plain or extensible (the SubFormat GUID must be PCM or IEEE float; anything else is
//  refused naming the GUID). Everything else (8-bit PCM, 64-bit float, ADPCM, mono, > 2 channels) is refused.
//  SigMF datatypes supported: cf32_le, cf32_be, ci16_le, ci16_be, cu16_le, cu16_be, ci8, cu8. Refused: every
//  real-valued type (r...), cf64_*, ci32_*, cu32_*, anything unknown. core:header_bytes / non-conforming
//  datasets are not supported (the data file is read from byte 0).
//
// DATA AND SIZES
//  A WAV data chunk size of 0 or 0xFFFFFFFF in a plain RIFF file (streaming writers, files over 4 GB) is read as
//  "to the end of the file"; in RF64 the ds64 value is used. A declared size larger than the file is clamped to
//  what is there and `info().truncated` is set. A trailing partial frame is ignored and counted in
//  `trailingBytes`. All sizes are 64-bit.
//
// THREADS AND ALLOCATION. One reader is used by one thread at a time. open*() allocate (read buffer of 128 KiB at
// most, SigMF metadata of at most 8 MiB); read()/seek() do not allocate. Not wired to any thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::source::iqfile {

enum class Kind : int { U8, S8, U16, S16, S24, S32, F32 };

struct Layout {
    Kind kind = Kind::F32;
    bool bigEndian = false;
    std::size_t bytesPerComponent() const {
        switch (kind) {
            case Kind::U8: case Kind::S8: return 1;
            case Kind::U16: case Kind::S16: return 2;
            case Kind::S24: return 3;
            case Kind::S32: case Kind::F32: return 4;
        }
        return 4;
    }
};

struct Capture {
    std::uint64_t sampleStart = 0;
    bool hasFrequency = false;
    double frequencyHz = 0.0;
    std::string datetime;
};

struct Annotation {
    std::uint64_t sampleStart = 0;
    bool hasCount = false;
    std::uint64_t sampleCount = 0;
    bool hasFreqEdges = false;
    double freqLowerHz = 0.0, freqUpperHz = 0.0;
    std::string label;
};

struct IqInfo {
    std::string container;                 // "raw", "wav", "rf64", "sigmf"
    std::string format;                    // "cu8", "cs16", "cf32", "pcm16", "pcm24", "pcm32", "float32", "cf32_le", ...
    double sampleRateHz = 0.0;
    bool sampleRateKnown = false;
    double centerFrequencyHz = 0.0;        // SigMF: first capture segment; raw: the parameter; WAV: 0 unless guessed
    bool centerKnown = false;
    std::uint64_t totalSamples = 0;        // complex samples (frames)
    std::uint64_t trailingBytes = 0;       // a partial last frame, ignored
    bool truncated = false;                // the header declared more data than the file holds
    bool guessedFromName = false;          // rate and / or centre came from guessFromFileName(), not from the file
    bool hasChannelMask = false;           // WAV extensible
    std::uint32_t channelMask = 0;
    std::uint16_t validBits = 0;           // WAV: valid bits per sample (container may be wider)
    std::vector<Capture> captures;         // SigMF, in file order
    std::vector<Annotation> annotations;   // SigMF, in file order
};

struct RawParams {
    double sampleRateHz = 0.0;             // raw files carry none: required unless useNameGuess finds one
    double centerFrequencyHz = 0.0;
    bool hasCenter = false;
    bool useNameGuess = false;             // fill a missing rate / centre from the file name (marked as a guess)
};

// A GUESS from a file name; never authoritative. `rule` names what matched.
struct NameGuess {
    bool hasFrequency = false, hasRate = false;
    double frequencyHz = 0.0, rateHz = 0.0;
    std::string rule;
};

namespace detail {

inline std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t u64(const unsigned char* p) { return static_cast<std::uint64_t>(u32(p)) | (static_cast<std::uint64_t>(u32(p + 4)) << 32); }

inline float finite(float v) { return std::isfinite(v) ? v : 0.0f; }

constexpr std::size_t kChunkFrames = 16384;
constexpr std::size_t kMaxMetaBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxListEntries = 65536;
constexpr std::size_t kMaxHeaderChunks = 256;

}  // namespace detail

class IqFileReader {
public:
    const IqInfo& info() const { return info_; }
    std::uint64_t position() const { return pos_; }

    // Moves to complex sample `sample` (0 .. totalSamples; the end is allowed). False if past the end.
    bool seek(std::uint64_t sample) {
        if (sample > info_.totalSamples) { return false; }
        pos_ = sample;
        return true;
    }

    // Reads up to n complex samples; returns how many (0 at the end of the data). Non-finite floats read as 0.
    std::size_t read(std::complex<float>* dst, std::size_t n) {
        const std::uint64_t left = info_.totalSamples - pos_;
        std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(n, left));
        std::size_t done = 0;
        const std::size_t fb = frameBytes_;
        while (done < want) {
            const std::size_t chunk = std::min(want - done, detail::kChunkFrames);
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(dataOffset_ + (pos_ + done) * fb));
            file_.read(reinterpret_cast<char*>(buf_.data()), static_cast<std::streamsize>(chunk * fb));
            const std::size_t gotFrames = static_cast<std::size_t>(file_.gcount()) / fb;
            decode(buf_.data(), dst + done, gotFrames);
            done += gotFrames;
            if (gotFrames < chunk) {   // the file shrank underneath us: stop, and say so by a short read
                info_.totalSamples = pos_ + done;
                info_.truncated = true;
                break;
            }
        }
        pos_ += done;
        return done;
    }

    // ---- construction (use the open* functions) ----
    bool init(const std::string& path, std::uint64_t dataOffset, std::uint64_t frames, Layout layout, std::string* err) {
        layout_ = layout;
        frameBytes_ = 2 * layout.bytesPerComponent();
        dataOffset_ = dataOffset;
        info_.totalSamples = frames;
        file_.open(std::filesystem::path(path), std::ios::binary);
        if (!file_) {
            if (err != nullptr) { *err = "cannot open " + path; }
            return false;
        }
        buf_.assign(detail::kChunkFrames * frameBytes_, 0);
        return true;
    }
    IqInfo& mutableInfo() { return info_; }

private:
    void decode(const unsigned char* p, std::complex<float>* dst, std::size_t frames) const {
        const std::size_t comps = frames * 2;
        float* o = reinterpret_cast<float*>(dst);
        const bool be = layout_.bigEndian;
        switch (layout_.kind) {
            case Kind::U8:
                for (std::size_t i = 0; i < comps; ++i) { o[i] = (static_cast<float>(p[i]) - 127.5f) / 127.5f; }
                break;
            case Kind::S8:
                for (std::size_t i = 0; i < comps; ++i) { o[i] = static_cast<float>(static_cast<std::int8_t>(p[i])) / 128.0f; }
                break;
            case Kind::U16:
                for (std::size_t i = 0; i < comps; ++i) {
                    const std::uint16_t v = be ? static_cast<std::uint16_t>((p[2 * i] << 8) | p[2 * i + 1]) : detail::u16(p + 2 * i);
                    o[i] = (static_cast<float>(v) - 32768.0f) / 32768.0f;
                }
                break;
            case Kind::S16:
                for (std::size_t i = 0; i < comps; ++i) {
                    const std::uint16_t v = be ? static_cast<std::uint16_t>((p[2 * i] << 8) | p[2 * i + 1]) : detail::u16(p + 2 * i);
                    o[i] = static_cast<float>(static_cast<std::int16_t>(v)) / 32768.0f;
                }
                break;
            case Kind::S24:
                for (std::size_t i = 0; i < comps; ++i) {
                    const unsigned char* q = p + 3 * i;
                    std::int32_t v = be ? ((q[0] << 16) | (q[1] << 8) | q[2]) : (q[0] | (q[1] << 8) | (q[2] << 16));
                    if ((v & 0x800000) != 0) { v -= 0x1000000; }
                    o[i] = static_cast<float>(v) / 8388608.0f;
                }
                break;
            case Kind::S32:
                for (std::size_t i = 0; i < comps; ++i) {
                    const unsigned char* q = p + 4 * i;
                    const std::uint32_t v = be ? ((static_cast<std::uint32_t>(q[0]) << 24) | (static_cast<std::uint32_t>(q[1]) << 16) |
                                                  (static_cast<std::uint32_t>(q[2]) << 8) | q[3])
                                               : detail::u32(q);
                    o[i] = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(v)) / 2147483648.0);
                }
                break;
            case Kind::F32:
                for (std::size_t i = 0; i < comps; ++i) {
                    const unsigned char* q = p + 4 * i;
                    const std::uint32_t v = be ? ((static_cast<std::uint32_t>(q[0]) << 24) | (static_cast<std::uint32_t>(q[1]) << 16) |
                                                  (static_cast<std::uint32_t>(q[2]) << 8) | q[3])
                                               : detail::u32(q);
                    o[i] = detail::finite(std::bit_cast<float>(v));
                }
                break;
        }
    }

    IqInfo info_;
    Layout layout_;
    std::size_t frameBytes_ = 8;
    std::uint64_t dataOffset_ = 0;
    std::uint64_t pos_ = 0;
    std::ifstream file_;
    std::vector<unsigned char> buf_;
};

namespace detail {

inline bool fail(std::string* err, const std::string& msg) {
    if (err != nullptr) { *err = msg; }
    return false;
}

inline std::string lower(std::string s) {
    for (auto& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// The GUID as text, from the 16 bytes in file order (Data1, Data2, Data3 little-endian, Data4 as bytes).
inline std::string guidText(const unsigned char* g) {
    char b[64];
    std::snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", u32(g), u16(g + 4), u16(g + 6), g[8], g[9], g[10], g[11], g[12],
                  g[13], g[14], g[15]);
    return b;
}

// Reads up to n bytes at `off`; returns the count.
inline std::size_t readAt(std::ifstream& f, std::uint64_t off, unsigned char* dst, std::size_t n) {
    f.clear();
    f.seekg(static_cast<std::streamoff>(off));
    f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(f.gcount());
}

inline bool fileSize(const std::string& path, std::uint64_t* size, std::string* err) {
    std::error_code ec;
    const auto s = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) { return fail(err, "cannot open " + path + ": " + ec.message()); }
    *size = s;
    return true;
}

}  // namespace detail

// ---- name guessing ------------------------------------------------------------------------------------------------------

inline NameGuess guessFromFileName(const std::string& path) {
    NameGuess g;
    std::string base = std::filesystem::path(path).stem().string();
    // tokens split on _ - space and the dot of a name like "145.5MHz" is kept inside the token
    std::vector<std::string> tok;
    std::string cur;
    for (char c : base) {
        if (c == '_' || c == '-' || c == ' ') {
            if (!cur.empty()) { tok.push_back(cur); }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) { tok.push_back(cur); }
    std::vector<double> bare;
    for (const std::string& t : tok) {
        std::size_t i = 0;
        while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) != 0 || t[i] == '.')) { ++i; }
        if (i == 0 || i > 20) { continue; }
        const std::string num = t.substr(0, i);
        const std::string suf = detail::lower(t.substr(i));
        double v = 0.0;
        try { v = std::stod(num); } catch (...) { continue; }
        if (!std::isfinite(v) || v <= 0.0) { continue; }
        if (suf == "ghz" || suf == "mhz" || suf == "khz") {
            if (!g.hasFrequency) {
                g.hasFrequency = true;
                g.frequencyHz = v * (suf == "ghz" ? 1e9 : suf == "mhz" ? 1e6 : 1e3);
                g.rule += "frequency from the unit-suffixed token '" + t + "'; ";
            }
        } else if (suf == "msps" || suf == "ksps" || suf == "sps") {
            if (!g.hasRate) {
                g.hasRate = true;
                g.rateHz = v * (suf == "msps" ? 1e6 : suf == "ksps" ? 1e3 : 1.0);
                g.rule += "sample rate from the unit-suffixed token '" + t + "'; ";
            }
        } else if (suf == "hz" && num.find('.') == std::string::npos && num.size() >= 6) {
            if (!g.hasFrequency) {
                g.hasFrequency = true;
                g.frequencyHz = v;
                g.rule += "frequency from the token '" + t + "' (digits then Hz, six or more digits); ";
            }
        } else if (suf.empty() && num.find('.') == std::string::npos && num.size() >= 5) {
            // bare integer: skip a date (yyyymmdd) and a time (hhmmss)
            const long long iv = static_cast<long long>(v);
            const bool date = num.size() == 8 && iv / 10000 >= 1900 && iv / 10000 <= 2200 && (iv / 100) % 100 >= 1 && (iv / 100) % 100 <= 12 &&
                              iv % 100 >= 1 && iv % 100 <= 31;
            const bool time = num.size() == 6 && iv / 10000 < 24 && (iv / 100) % 100 < 60 && iv % 100 < 60;
            if (!date && !time) { bare.push_back(v); }
        }
    }
    if (!g.hasFrequency && !g.hasRate && bare.size() >= 2) {
        g.hasFrequency = true;
        g.hasRate = true;
        g.frequencyHz = bare[0];
        g.rateHz = bare[1];
        g.rule = "two bare integers in the name: first taken as the frequency in Hz, second as the sample rate in Hz (a convention some recorders use; a guess)";
    } else if (!g.hasRate && g.hasFrequency && bare.size() == 1) {
        g.hasRate = true;
        g.rateHz = bare[0];
        g.rule += "the one remaining bare integer taken as the sample rate in Hz (a guess); ";
    }
    if (g.hasRate && (g.rateHz < 100.0 || g.rateHz > 1e9)) { g.hasRate = false; }
    return g;
}

// ---- raw ---------------------------------------------------------------------------------------------------------------------

inline std::unique_ptr<IqFileReader> openRaw(const std::string& path, Layout layout, const RawParams& params, std::string* err) {
    RawParams p = params;
    bool guessed = false;
    if (p.useNameGuess) {
        const NameGuess g = guessFromFileName(path);
        if (!(p.sampleRateHz > 0.0) && g.hasRate) { p.sampleRateHz = g.rateHz; guessed = true; }
        if (!p.hasCenter && g.hasFrequency) { p.centerFrequencyHz = g.frequencyHz; p.hasCenter = true; guessed = true; }
    }
    if (!(p.sampleRateHz > 0.0) || !std::isfinite(p.sampleRateHz)) {
        detail::fail(err, "raw I/Q files carry no sample rate: supply one (" + path + ")");
        return nullptr;
    }
    std::uint64_t size = 0;
    if (!detail::fileSize(path, &size, err)) { return nullptr; }
    auto r = std::make_unique<IqFileReader>();
    const std::size_t fb = 2 * layout.bytesPerComponent();
    if (!r->init(path, 0, size / fb, layout, err)) { return nullptr; }
    IqInfo& i = r->mutableInfo();
    i.container = "raw";
    i.format = layout.kind == Kind::U8 ? "cu8" : layout.kind == Kind::S16 ? "cs16" : "cf32";
    i.sampleRateHz = p.sampleRateHz;
    i.sampleRateKnown = true;
    i.centerFrequencyHz = p.centerFrequencyHz;
    i.centerKnown = p.hasCenter;
    i.trailingBytes = size % fb;
    i.guessedFromName = guessed;
    return r;
}

// ---- WAV / RF64 -----------------------------------------------------------------------------------------------------------------

inline std::unique_ptr<IqFileReader> openWav(const std::string& path, std::string* err) {
    std::uint64_t fsize = 0;
    if (!detail::fileSize(path, &fsize, err)) { return nullptr; }
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) { detail::fail(err, "cannot open " + path); return nullptr; }
    unsigned char h[12];
    if (detail::readAt(f, 0, h, 12) < 12) { detail::fail(err, "too short to be a WAV file: " + path); return nullptr; }
    const bool riff = std::memcmp(h, "RIFF", 4) == 0;
    const bool rf64 = std::memcmp(h, "RF64", 4) == 0 || std::memcmp(h, "BW64", 4) == 0;
    if ((!riff && !rf64) || std::memcmp(h + 8, "WAVE", 4) != 0) { detail::fail(err, "not a RIFF/RF64 WAVE file: " + path); return nullptr; }

    bool haveFmt = false, haveData = false, haveDs64 = false;
    std::uint64_t ds64Data = 0;
    std::uint16_t tag = 0, channels = 0, bits = 0, blockAlign = 0, validBits = 0;
    std::uint32_t rate = 0, mask = 0;
    bool extensible = false;
    unsigned char guid[16] = {};
    std::uint64_t dataOff = 0, dataSize = 0;
    bool dataToEof = false;
    std::uint64_t pos = 12;
    std::size_t walked = 0;
    while (pos + 8 <= fsize && walked < detail::kMaxHeaderChunks) {
        ++walked;
        unsigned char ch[8];
        if (detail::readAt(f, pos, ch, 8) < 8) { break; }
        const std::uint32_t size = detail::u32(ch + 4);
        const std::uint64_t body = pos + 8;
        if (std::memcmp(ch, "ds64", 4) == 0 && !haveDs64) {
            unsigned char d[24];
            if (size < 24 || detail::readAt(f, body, d, 24) < 24) { detail::fail(err, "ds64 chunk shorter than its three 64-bit fields"); return nullptr; }
            ds64Data = detail::u64(d + 8);
            haveDs64 = true;
        } else if (std::memcmp(ch, "fmt ", 4) == 0 && !haveFmt) {
            unsigned char b[40] = {};
            const std::size_t got = detail::readAt(f, body, b, std::min<std::size_t>(size, 40));
            if (got < 16) { detail::fail(err, "fmt chunk shorter than 16 bytes"); return nullptr; }
            tag = detail::u16(b);
            channels = detail::u16(b + 2);
            rate = detail::u32(b + 4);
            blockAlign = detail::u16(b + 12);
            bits = detail::u16(b + 14);
            if (tag == 0xFFFE) {
                if (got < 40) { detail::fail(err, "WAVE_FORMAT_EXTENSIBLE fmt chunk shorter than 40 bytes"); return nullptr; }
                const std::uint16_t cb = detail::u16(b + 16);
                if (cb < 22) { detail::fail(err, "WAVE_FORMAT_EXTENSIBLE with cbSize " + std::to_string(cb) + " (needs 22)"); return nullptr; }
                extensible = true;
                validBits = detail::u16(b + 18);
                mask = detail::u32(b + 20);
                std::memcpy(guid, b + 24, 16);
            }
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0 && !haveData) {
            dataOff = body;
            haveData = true;
            if (size == 0xFFFFFFFFu && rf64 && haveDs64) {
                dataSize = ds64Data;
            } else if (size == 0xFFFFFFFFu || size == 0) {
                dataToEof = true;
                dataSize = fsize > body ? fsize - body : 0;
            } else {
                dataSize = size;
            }
            if (haveFmt) { break; }
            if (dataToEof) { break; }
        }
        pos = body + size + (size & 1u);
    }
    if (!haveFmt) { detail::fail(err, "no fmt chunk found (" + path + ")"); return nullptr; }
    if (!haveData) { detail::fail(err, "no data chunk found (" + path + ")"); return nullptr; }

    Layout layout;
    std::string fmtName;
    std::uint16_t effTag = tag;
    if (extensible) {
        static const unsigned char kTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        // GUID bytes: Data1 (LE u32), Data2, Data3, Data4. For the two used here Data1 is the format tag and the rest is the base tail.
        const bool baseGuid = std::memcmp(guid + 2, kTail, 14) == 0;
        const std::uint16_t sub = detail::u16(guid);
        if (!baseGuid || (sub != 1 && sub != 3)) {
            detail::fail(err, "unsupported WAVE_FORMAT_EXTENSIBLE SubFormat " + detail::guidText(guid) + " (only PCM {00000001-0000-0010-8000-00AA00389B71} and IEEE float {00000003-...})");
            return nullptr;
        }
        effTag = sub;
    }
    if (channels != 2) { detail::fail(err, "I/Q WAV files need 2 channels (I, Q); this one has " + std::to_string(channels)); return nullptr; }
    if (rate == 0) { detail::fail(err, "WAV header declares a sample rate of 0"); return nullptr; }
    if (effTag == 1) {
        if (bits == 16) { layout.kind = Kind::S16; fmtName = "pcm16"; }
        else if (bits == 24) { layout.kind = Kind::S24; fmtName = "pcm24"; }
        else if (bits == 32) { layout.kind = Kind::S32; fmtName = "pcm32"; }
        else { detail::fail(err, "unsupported PCM bit depth " + std::to_string(bits) + " (16, 24 and 32 are supported)"); return nullptr; }
    } else if (effTag == 3) {
        if (bits != 32) { detail::fail(err, "unsupported IEEE float bit depth " + std::to_string(bits) + " (only 32)"); return nullptr; }
        layout.kind = Kind::F32;
        fmtName = "float32";
    } else {
        detail::fail(err, "unsupported WAV format tag " + std::to_string(effTag) + " (only PCM = 1, IEEE float = 3, and extensible carrying those)");
        return nullptr;
    }
    const std::size_t fb = 2 * layout.bytesPerComponent();
    if (blockAlign != fb) { detail::fail(err, "fmt block align " + std::to_string(blockAlign) + " does not match " + std::to_string(fb) + " bytes per I/Q frame"); return nullptr; }
    if (extensible && validBits > bits) { detail::fail(err, "valid bits per sample exceeds the container size"); return nullptr; }

    const std::uint64_t avail = fsize > dataOff ? fsize - dataOff : 0;
    const std::uint64_t usable = std::min(dataSize, avail);
    auto r = std::make_unique<IqFileReader>();
    if (!r->init(path, dataOff, usable / fb, layout, err)) { return nullptr; }
    IqInfo& i = r->mutableInfo();
    i.container = rf64 ? "rf64" : "wav";
    i.format = fmtName;
    i.sampleRateHz = rate;
    i.sampleRateKnown = true;
    i.totalSamples = usable / fb;
    i.trailingBytes = usable % fb;
    i.truncated = dataSize > avail;
    i.hasChannelMask = extensible;
    i.channelMask = mask;
    i.validBits = extensible ? validBits : bits;
    return r;
}

// ---- SigMF ---------------------------------------------------------------------------------------------------------------------

namespace detail {

inline bool sigmfLayout(const std::string& dt, Layout* l, std::string* name, std::string* err) {
    if (dt.empty() || (dt[0] != 'c' && dt[0] != 'r')) { return fail(err, "SigMF: unknown core:datatype '" + dt + "'"); }
    if (dt[0] == 'r') { return fail(err, "SigMF: real-valued datatype '" + dt + "' is not an I/Q format and is not supported"); }
    struct E { const char* n; Kind k; bool be; };
    static const E table[] = {{"cf32_le", Kind::F32, false}, {"cf32_be", Kind::F32, true}, {"ci16_le", Kind::S16, false}, {"ci16_be", Kind::S16, true},
                              {"cu16_le", Kind::U16, false}, {"cu16_be", Kind::U16, true}, {"ci8", Kind::S8, false}, {"cu8", Kind::U8, false}};
    for (const E& e : table) {
        if (dt == e.n) { l->kind = e.k; l->bigEndian = e.be; *name = dt; return true; }
    }
    return fail(err, "SigMF: datatype '" + dt + "' is not supported (supported: cf32_le, cf32_be, ci16_le, ci16_be, cu16_le, cu16_be, ci8, cu8)");
}

inline bool getU64(const nlohmann::json& o, const char* key, std::uint64_t* out) {
    const auto it = o.find(key);
    if (it == o.end()) { return false; }
    if (it->is_number_unsigned()) { *out = it->get<std::uint64_t>(); return true; }
    if (it->is_number_integer()) { const auto v = it->get<std::int64_t>(); if (v < 0) { return false; } *out = static_cast<std::uint64_t>(v); return true; }
    if (it->is_number_float()) { const double d = it->get<double>(); if (d < 0 || d > 1.8e19 || std::floor(d) != d) { return false; } *out = static_cast<std::uint64_t>(d); return true; }
    return false;
}
inline bool getNum(const nlohmann::json& o, const char* key, double* out) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) { return false; }
    *out = it->get<double>();
    return std::isfinite(*out);
}
inline std::string getStr(const nlohmann::json& o, const char* key) {
    const auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}  // namespace detail

// `metaPath` ends in .sigmf-meta (or .sigmf-data: the sibling meta is used). fallbackRateHz is used only when the
// metadata has no core:sample_rate.
inline std::unique_ptr<IqFileReader> openSigmf(const std::string& path, double fallbackRateHz, std::string* err) {
    std::filesystem::path meta = std::filesystem::path(path);
    if (meta.extension() == ".sigmf-data") { meta.replace_extension(".sigmf-meta"); }
    std::filesystem::path data = meta;
    data.replace_extension(".sigmf-data");
    std::uint64_t msize = 0;
    if (!detail::fileSize(meta.string(), &msize, err)) { return nullptr; }
    if (msize > detail::kMaxMetaBytes) { detail::fail(err, "SigMF metadata file is larger than 8 MiB"); return nullptr; }
    std::string text(static_cast<std::size_t>(msize), '\0');
    {
        std::ifstream mf(meta, std::ios::binary);
        if (!mf) { detail::fail(err, "cannot open " + meta.string()); return nullptr; }
        mf.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(mf.gcount()));
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { detail::fail(err, "SigMF metadata is not valid JSON: " + meta.string()); return nullptr; }
    const auto gi = j.find("global");
    if (gi == j.end() || !gi->is_object()) { detail::fail(err, "SigMF metadata has no \"global\" object"); return nullptr; }
    const nlohmann::json& g = *gi;
    if (!g.contains("core:datatype") || !g["core:datatype"].is_string()) { detail::fail(err, "SigMF metadata is missing global core:datatype"); return nullptr; }
    Layout layout;
    std::string fmtName;
    if (!detail::sigmfLayout(g["core:datatype"].get<std::string>(), &layout, &fmtName, err)) { return nullptr; }

    IqInfo info;
    info.container = "sigmf";
    info.format = fmtName;
    double sr = 0.0;
    if (detail::getNum(g, "core:sample_rate", &sr) && sr > 0.0) {
        info.sampleRateHz = sr;
        info.sampleRateKnown = true;
    } else if (fallbackRateHz > 0.0 && std::isfinite(fallbackRateHz)) {
        info.sampleRateHz = fallbackRateHz;
        info.sampleRateKnown = true;
    }
    const auto ci = j.find("captures");
    if (ci != j.end() && ci->is_array()) {
        if (ci->size() > detail::kMaxListEntries) { detail::fail(err, "SigMF metadata has more than 65536 capture segments"); return nullptr; }
        for (const auto& c : *ci) {
            if (!c.is_object()) { continue; }
            Capture cap;
            if (!detail::getU64(c, "core:sample_start", &cap.sampleStart)) { detail::fail(err, "SigMF capture segment without a valid core:sample_start"); return nullptr; }
            cap.hasFrequency = detail::getNum(c, "core:frequency", &cap.frequencyHz);
            cap.datetime = detail::getStr(c, "core:datetime");
            info.captures.push_back(cap);
        }
        if (!info.captures.empty() && info.captures.front().hasFrequency) {
            info.centerFrequencyHz = info.captures.front().frequencyHz;
            info.centerKnown = true;
        }
    }
    const auto ai = j.find("annotations");
    if (ai != j.end() && ai->is_array()) {
        if (ai->size() > detail::kMaxListEntries) { detail::fail(err, "SigMF metadata has more than 65536 annotations"); return nullptr; }
        for (const auto& a : *ai) {
            if (!a.is_object()) { continue; }
            Annotation an;
            if (!detail::getU64(a, "core:sample_start", &an.sampleStart)) { continue; }   // a malformed annotation is skipped, not fatal
            an.hasCount = detail::getU64(a, "core:sample_count", &an.sampleCount);
            double lo = 0, hi = 0;
            if (detail::getNum(a, "core:freq_lower_edge", &lo) && detail::getNum(a, "core:freq_upper_edge", &hi)) {
                an.hasFreqEdges = true;
                an.freqLowerHz = lo;
                an.freqUpperHz = hi;
            }
            an.label = detail::getStr(a, "core:label");
            info.annotations.push_back(an);
        }
    }

    std::uint64_t dsize = 0;
    if (!detail::fileSize(data.string(), &dsize, err)) { return nullptr; }
    const std::size_t fb = 2 * layout.bytesPerComponent();
    auto r = std::make_unique<IqFileReader>();
    if (!r->init(data.string(), 0, dsize / fb, layout, err)) { return nullptr; }
    info.totalSamples = dsize / fb;
    info.trailingBytes = dsize % fb;
    r->mutableInfo() = info;
    return r;
}

// ---- dispatch --------------------------------------------------------------------------------------------------------------------

// By extension: .wav -> WAV/RF64; .sigmf-meta / .sigmf-data -> SigMF; .cu8 / .cs16 / .cf32 -> raw (params needed).
inline std::unique_ptr<IqFileReader> openAuto(const std::string& path, const RawParams& params, std::string* err) {
    const std::string ext = detail::lower(std::filesystem::path(path).extension().string());
    if (ext == ".wav" || ext == ".rf64") { return openWav(path, err); }
    if (ext == ".sigmf-meta" || ext == ".sigmf-data") { return openSigmf(path, params.sampleRateHz, err); }
    if (ext == ".cu8") { Layout l; l.kind = Kind::U8; return openRaw(path, l, params, err); }
    if (ext == ".cs16") { Layout l; l.kind = Kind::S16; return openRaw(path, l, params, err); }
    if (ext == ".cf32") { Layout l; l.kind = Kind::F32; return openRaw(path, l, params, err); }
    detail::fail(err, "unrecognised I/Q file extension '" + ext + "' (known: .wav, .sigmf-meta, .sigmf-data, .cu8, .cs16, .cf32)");
    return nullptr;
}

}  // namespace cascade::source::iqfile
