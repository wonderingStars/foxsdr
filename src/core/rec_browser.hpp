// Recordings browser model: scans a folder for I/Q and audio recordings, reads each file's HEADER only (rate, centre frequency, length, start time
// where the format or the name carries one), and offers sort and filter as pure functions on the result. The scan runs on its own thread, publishes
// partial results as it goes and never blocks the caller (the screen polls a snapshot).
//
// STANDALONE. Nothing here is wired (docs/feature-records/recording-models.md, Part 4). It reads through the I/Q readers of the previous batch
// (source/iq_readers.hpp: raw cu8/cs16/cf32, WAV and RF64, SigMF) and a small audio probe written here (WAV of any channel count and PCM / float
// format, and the STREAMINFO block of FLAC, RFC 9639 section 8.2: sample rate, channels, bits, total samples).
//
// WHAT IS A FILE'S KIND. SigMF, raw cu8/cs16/cf32 and one-channel WAV are unambiguous (SigMF and raw are I/Q, one-channel WAV is audio). A TWO-channel
// WAV can be a stereo audio recording or an I/Q recording; the header does not say. The kind is then GUESSED (kindGuessed = true) from the file name
// (a token "iq" or a known recorder prefix: SDRSharp, HDSDR, SDRuno, SDRconnect) and the sample rate (above 48 kHz is I/Q); otherwise it is Audio. FLAC is
// always Audio.
//
// START TIME, in this order of trust: the SigMF core:datetime of the first capture (UTC); a date-time in the file name (compact YYYYMMDD_HHMMSS with an
// optional Z, or YYYY-MM-DD_HH-MM-SS; a name without Z is taken as LOCAL time and converted with the caller's offset, flagged startLocalGuess);
// the file's modified time minus its duration (flagged as an estimate); else unknown. startSource says which.
//
// HOSTILE FILES. Every file is probed inside its own try/catch and a failure becomes an entry with broken = true and the error text; zero-byte files,
// truncated headers, junk with a .wav name, a directory named x.wav, a file that disappears during the scan, a huge sparse file: none stops the scan.
// Directory links are not followed. The list is capped (maxEntries) and says so.
//
// THREADS. RecordingsBrowser: start(), cancel(), snapshot() from any thread (a mutex held only for a pointer swap, never during I/O); one worker thread
// does all reading. sortAndFilter() is pure.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/civil_time.hpp"
#include "core/if_recorder.hpp"
#include "source/iq_readers.hpp"

namespace cascade::core::recbrowse {

enum class Kind : int { Iq = 0, Audio = 1 };
enum class StartSource : int { None = 0, SigmfDatetime = 1, FileNameUtc = 2, FileNameLocal = 3, ModifiedMinusDuration = 4, IfHeader = 5 };

struct Entry {
    std::string path;
    std::string fileName;
    std::uint64_t sizeBytes = 0;
    std::int64_t modifiedUnixMs = 0;
    Kind kind = Kind::Audio;
    bool kindGuessed = false;
    std::string container;               // "raw", "wav", "rf64", "sigmf", "flac"
    std::string format;                  // "cu8", "pcm16", "float32", "flac16", ...
    double sampleRateHz = 0.0;
    bool sampleRateKnown = false;
    double centerHz = 0.0;
    bool centerKnown = false;
    int channels = 0;
    int bitsPerSample = 0;
    std::uint64_t totalFrames = 0;
    double durationS = 0.0;              // 0 when the rate is unknown
    bool durationKnown = false;
    std::int64_t startUnixMs = 0;
    StartSource startSource = StartSource::None;
    bool truncated = false;              // the header declared more data than the file holds
    bool guessedFromName = false;        // rate / centre of a raw file came from the name
    bool ifRecording = false;            // a FoxSDR channel (IF) recording: the "fxif" chunk was read
    std::string mode, radio;             // from the fxif chunk
    double bandwidthHz = 0.0;
    bool broken = false;
    std::string error;
};

struct ScanOptions {
    int maxDepth = 3;                         // 0 = the folder only
    std::size_t maxEntries = 20000;
    int localUtcOffsetMinutes = 0;            // for names without a Z
    bool guessRawFromName = true;
};

struct Snapshot {
    std::shared_ptr<const std::vector<Entry>> entries = std::make_shared<const std::vector<Entry>>();
    std::size_t candidates = 0;               // files found with a recording extension
    std::size_t probed = 0;
    std::size_t ignored = 0;                  // files with another extension
    std::size_t broken = 0;
    bool done = false;
    bool cancelled = false;
    bool listTruncated = false;               // more candidates than maxEntries
    std::string folderError;                  // the folder could not be opened
    std::uint64_t scanId = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------------------------------------------------------------------------
namespace detail {

inline std::string lowerStr(std::string s) { for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } } return s; }
inline bool digits(const std::string& s, std::size_t at, std::size_t n) {
    if (at + n > s.size()) { return false; }
    for (std::size_t i = 0; i < n; ++i) { if (s[at + i] < '0' || s[at + i] > '9') { return false; } }
    return true;
}
inline int num(const std::string& s, std::size_t at, std::size_t n) { int v = 0; for (std::size_t i = 0; i < n; ++i) { v = v * 10 + (s[at + i] - '0'); } return v; }
inline bool validDateTime(int y, int mo, int d, int h, int mi, int s) {
    if (y < 1990 || y > 2100 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59 || s > 60) { return false; }
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int lim = dim[mo - 1] + ((mo == 2 && civil::isLeap(y)) ? 1 : 0);
    return d <= lim;
}

}  // namespace detail

struct NameTime { bool found = false; std::int64_t unixMs = 0; bool utc = false; };

// A date and time in a file name: YYYYMMDD<sep>HHMMSS[Z]  or  YYYY-MM-DD<sep>HH-MM-SS (sep: _ - space T, or nothing for the compact form).
// A name without Z is returned with unixMs as if it were UTC and utc = false (the caller applies its offset).
inline NameTime parseNameTime(const std::string& name) {
    NameTime r;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') { continue; }   // start of a digit run only
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        std::size_t end = 0;
        bool ok = false;
        if (detail::digits(name, i, 8)) {
            std::size_t j = i + 8;
            if (j < name.size() && (name[j] == '_' || name[j] == '-' || name[j] == ' ' || name[j] == 'T')) { ++j; }
            if (detail::digits(name, j, 6) && !(j + 6 < name.size() && name[j + 6] >= '0' && name[j + 6] <= '9')) {
                y = detail::num(name, i, 4); mo = detail::num(name, i + 4, 2); d = detail::num(name, i + 6, 2);
                h = detail::num(name, j, 2); mi = detail::num(name, j + 2, 2); s = detail::num(name, j + 4, 2);
                end = j + 6; ok = detail::validDateTime(y, mo, d, h, mi, s);
            }
        }
        if (!ok && detail::digits(name, i, 4) && i + 4 < name.size() && name[i + 4] == '-' && detail::digits(name, i + 5, 2) && i + 7 < name.size() && name[i + 7] == '-' &&
            detail::digits(name, i + 8, 2)) {
            std::size_t j = i + 10;
            if (j < name.size() && (name[j] == '_' || name[j] == '-' || name[j] == ' ' || name[j] == 'T')) {
                ++j;
                if (detail::digits(name, j, 2) && j + 2 < name.size() && (name[j + 2] == '-' || name[j + 2] == '.' || name[j + 2] == ':') && detail::digits(name, j + 3, 2) &&
                    j + 5 < name.size() && (name[j + 5] == '-' || name[j + 5] == '.' || name[j + 5] == ':') && detail::digits(name, j + 6, 2)) {
                    y = detail::num(name, i, 4); mo = detail::num(name, i + 5, 2); d = detail::num(name, i + 8, 2);
                    h = detail::num(name, j, 2); mi = detail::num(name, j + 3, 2); s = detail::num(name, j + 6, 2);
                    end = j + 8; ok = detail::validDateTime(y, mo, d, h, mi, s);
                }
            }
        }
        if (ok) {
            r.found = true;
            r.unixMs = civil::toUnixMs(y, mo, d, h, mi, std::min(s, 59));
            r.utc = end < name.size() && (name[end] == 'Z' || name[end] == 'z');
            return r;
        }
    }
    return r;
}

// SigMF core:datetime: "YYYY-MM-DDTHH:MM:SS[.fff...]Z". Returns false when it does not parse.
inline bool parseIso8601Utc(const std::string& s, std::int64_t* unixMs) {
    if (s.size() < 19 || !detail::digits(s, 0, 4) || s[4] != '-' || !detail::digits(s, 5, 2) || s[7] != '-' || !detail::digits(s, 8, 2) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') ||
        !detail::digits(s, 11, 2) || s[13] != ':' || !detail::digits(s, 14, 2) || s[16] != ':' || !detail::digits(s, 17, 2)) {
        return false;
    }
    const int y = detail::num(s, 0, 4), mo = detail::num(s, 5, 2), d = detail::num(s, 8, 2), h = detail::num(s, 11, 2), mi = detail::num(s, 14, 2), sec = detail::num(s, 17, 2);
    if (!detail::validDateTime(y, mo, d, h, mi, sec) && !(y >= 1970 && y < 1990 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && sec < 61)) { return false; }
    int ms = 0;
    std::size_t p = 19;
    if (p < s.size() && s[p] == '.') {
        ++p;
        int digitsRead = 0;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') { if (digitsRead < 3) { ms = ms * 10 + (s[p] - '0'); ++digitsRead; } ++p; }
        while (digitsRead > 0 && digitsRead < 3) { ms *= 10; ++digitsRead; }
    }
    // only UTC is defined by SigMF: accept "Z" or nothing; a numeric offset is refused rather than guessed
    if (p < s.size() && !(s[p] == 'Z' || s[p] == 'z')) { return false; }
    *unixMs = civil::toUnixMs(y, mo, d, h, mi, std::min(sec, 59), ms);
    return true;
}

namespace detail {

inline bool hasToken(const std::string& lowerName, const char* tok) {
    // the token delimited by non-alphanumerics
    const std::string t = tok;
    std::size_t at = 0;
    while ((at = lowerName.find(t, at)) != std::string::npos) {
        const bool leftOk = at == 0 || !(std::isalnum(static_cast<unsigned char>(lowerName[at - 1])) != 0);
        const std::size_t e = at + t.size();
        const bool rightOk = e >= lowerName.size() || !(std::isalnum(static_cast<unsigned char>(lowerName[e])) != 0);
        if (leftOk && rightOk) { return true; }
        ++at;
    }
    return false;
}

inline std::uint16_t rd16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t rd32(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
inline std::uint64_t rd64(const unsigned char* p) { return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32); }

struct AudioHead {
    bool ok = false;
    std::string error, container, format;
    int channels = 0, bits = 0;
    double rate = 0.0;
    std::uint64_t frames = 0;
    bool truncated = false;
    std::uint64_t dataOffset = 0;
    unsigned tag = 0, blockAlign = 0;
};

// WAV (RIFF / RF64 / BW64) of any channel count and PCM or float format: header only.
inline AudioHead probeWav(const std::string& path, std::uint64_t fileSize) {
    AudioHead h;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) { h.error = "cannot open the file"; return h; }
    unsigned char hd[12];
    f.read(reinterpret_cast<char*>(hd), 12);
    if (f.gcount() != 12) { h.error = "shorter than a WAV header"; return h; }
    const bool rf64 = std::memcmp(hd, "RF64", 4) == 0 || std::memcmp(hd, "BW64", 4) == 0;
    if (!rf64 && std::memcmp(hd, "RIFF", 4) != 0) { h.error = "not a RIFF/WAVE file"; return h; }
    if (std::memcmp(hd + 8, "WAVE", 4) != 0) { h.error = "RIFF file is not WAVE"; return h; }
    h.container = rf64 ? "rf64" : "wav";
    std::uint64_t pos = 12, ds64Data = 0;
    bool haveFmt = false, haveDs64 = false;
    unsigned tag = 0, blockAlign = 0;
    for (int chunk = 0; chunk < 256; ++chunk) {
        unsigned char ch[8];
        f.clear(); f.seekg(static_cast<std::streamoff>(pos));
        f.read(reinterpret_cast<char*>(ch), 8);
        if (f.gcount() != 8) { break; }
        const std::uint64_t size = rd32(ch + 4);
        const std::uint64_t body = pos + 8;
        if (std::memcmp(ch, "ds64", 4) == 0 && size >= 16) {
            unsigned char d[16];
            f.read(reinterpret_cast<char*>(d), 16);
            if (f.gcount() == 16) { ds64Data = rd64(d + 8); haveDs64 = true; }
        } else if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            unsigned char fm[40] = {};
            f.read(reinterpret_cast<char*>(fm), static_cast<std::streamsize>(std::min<std::uint64_t>(size, 40)));
            if (f.gcount() < 16) { h.error = "fmt chunk cut short"; return h; }
            tag = rd16(fm); h.channels = rd16(fm + 2); h.rate = rd32(fm + 4); blockAlign = rd16(fm + 12); h.bits = rd16(fm + 14);
            if (tag == 0xFFFE && size >= 40 && f.gcount() >= 26 + 2) { tag = rd16(fm + 24); }
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            if (!haveFmt) { h.error = "data chunk before the fmt chunk"; return h; }
            if (tag != 1 && tag != 3) { h.error = "unsupported WAVE format tag " + std::to_string(tag) + " (only PCM and IEEE float)"; return h; }
            if (h.channels < 1 || h.channels > 8 || h.rate <= 0.0 || blockAlign == 0) { h.error = "implausible fmt chunk"; return h; }
            h.format = tag == 3 ? "float" + std::to_string(h.bits) : "pcm" + std::to_string(h.bits);
            const std::uint64_t avail = fileSize > body ? fileSize - body : 0;
            std::uint64_t dsize = size;
            if (size == 0xFFFFFFFFull && rf64 && haveDs64) { dsize = ds64Data; }
            else if (size == 0xFFFFFFFFull || size == 0) { dsize = avail; }
            if (dsize > avail) { dsize = avail; h.truncated = true; }
            h.frames = dsize / blockAlign;
            h.dataOffset = body; h.tag = tag; h.blockAlign = blockAlign;
            h.ok = true;
            return h;
        }
        pos = body + size + (size & 1);
        if (pos >= fileSize) { break; }
    }
    h.error = haveFmt ? "no data chunk" : "no fmt chunk";
    return h;
}

// FLAC: the marker and STREAMINFO only (RFC 9639 section 8.2).
inline AudioHead probeFlac(const std::string& path) {
    AudioHead h;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) { h.error = "cannot open the file"; return h; }
    unsigned char b[42];
    f.read(reinterpret_cast<char*>(b), 42);
    if (f.gcount() != 42) { h.error = "shorter than a FLAC stream header"; return h; }
    if (std::memcmp(b, "fLaC", 4) != 0) { h.error = "not a FLAC file"; return h; }
    if ((b[4] & 0x7F) != 0 || b[5] != 0 || b[6] != 0 || b[7] != 34) { h.error = "the first metadata block is not STREAMINFO"; return h; }
    const unsigned char* s = b + 8;
    const std::uint64_t packed = (static_cast<std::uint64_t>(s[10]) << 56) | (static_cast<std::uint64_t>(s[11]) << 48) | (static_cast<std::uint64_t>(s[12]) << 40) | (static_cast<std::uint64_t>(s[13]) << 32) |
                                 (static_cast<std::uint64_t>(s[14]) << 24) | (static_cast<std::uint64_t>(s[15]) << 16) | (static_cast<std::uint64_t>(s[16]) << 8) | s[17];
    h.rate = static_cast<double>(packed >> 44);
    h.channels = static_cast<int>(((packed >> 41) & 7) + 1);
    h.bits = static_cast<int>(((packed >> 36) & 31) + 1);
    h.frames = packed & 0xFFFFFFFFFull;   // 0 = unknown (a file still being written, or cut by a crash)
    if (h.rate <= 0.0) { h.error = "STREAMINFO has a zero sample rate"; return h; }
    h.container = "flac";
    h.format = "flac" + std::to_string(h.bits);
    h.ok = true;
    return h;
}

}  // namespace detail

// Probes ONE file: never throws, never blocks beyond the file reads of its header. `ext` is the lowercase extension with its dot.
inline Entry probeFile(const std::filesystem::path& p, const ScanOptions& opt) {
    namespace fs = std::filesystem;
    Entry e;
    e.path = p.string();
    e.fileName = p.filename().string();
    try {
        std::error_code ec;
        e.sizeBytes = fs::file_size(p, ec);
        if (ec) { e.broken = true; e.error = "cannot read the file size: " + ec.message(); return e; }
        const auto ft = fs::last_write_time(p, ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            e.modifiedUnixMs = static_cast<std::int64_t>(sys.time_since_epoch().count());
        }
        const std::string ext = detail::lowerStr(p.extension().string());
        const std::string lname = detail::lowerStr(e.fileName);
        if (ext == ".flac") {
            const detail::AudioHead h = detail::probeFlac(e.path);
            e.kind = Kind::Audio; e.container = "flac";
            if (!h.ok) { e.broken = true; e.error = h.error; return e; }
            e.format = h.format; e.channels = h.channels; e.bitsPerSample = h.bits; e.sampleRateHz = h.rate; e.sampleRateKnown = true; e.totalFrames = h.frames;
            if (h.frames != 0) { e.durationKnown = true; e.durationS = static_cast<double>(h.frames) / h.rate; }
        } else if (ext == ".wav" || ext == ".rf64") {
            const detail::AudioHead h = detail::probeWav(e.path, e.sizeBytes);
            e.container = h.container.empty() ? "wav" : h.container;
            if (!h.ok) { e.broken = true; e.error = h.error; return e; }
            e.format = h.format; e.channels = h.channels; e.bitsPerSample = h.bits; e.sampleRateHz = h.rate; e.sampleRateKnown = true; e.totalFrames = h.frames; e.truncated = h.truncated;
            e.durationKnown = true; e.durationS = static_cast<double>(h.frames) / h.rate;
            if (h.channels == 1) { e.kind = Kind::Audio; }
            else if (h.channels == 2) {
                const bool nameSaysIq = detail::hasToken(lname, "iq") || lname.rfind("sdrsharp", 0) == 0 || lname.rfind("hdsdr", 0) == 0 || lname.rfind("sdruno", 0) == 0 ||
                                        lname.rfind("sdrconnect", 0) == 0 || lname.rfind("baseband", 0) == 0;
                e.kind = (nameSaysIq || h.rate > 48000.0) ? Kind::Iq : Kind::Audio;
                e.kindGuessed = true;
                // an I/Q reading also supplies the container details (centre is not in a plain WAV)
            } else { e.kind = Kind::Audio; }
            if (h.channels == 2) {   // a FoxSDR channel recording says what it is: kind, centre, mode, bandwidth, start time
                const ifrec::IfHeader ih = ifrec::readIfHeader(e.path);
                if (ih.ok && ih.hasMeta) {
                    e.kind = Kind::Iq; e.kindGuessed = false; e.ifRecording = true;
                    e.centerHz = ih.meta.centerHz; e.centerKnown = true; e.mode = ih.meta.mode; e.radio = ih.meta.radio; e.bandwidthHz = ih.meta.bandwidthHz;
                    if (ih.meta.startUnixMs != 0) { e.startUnixMs = ih.meta.startUnixMs; e.startSource = StartSource::IfHeader; }
                }
            }
        } else if (ext == ".sigmf-meta" || ext == ".cu8" || ext == ".cs16" || ext == ".cf32" || ext == ".sigmf-data") {
            e.kind = Kind::Iq;
            source::iqfile::RawParams rp;
            rp.useNameGuess = opt.guessRawFromName;
            std::string err;
            auto r = source::iqfile::openAuto(e.path, rp, &err);
            if (r == nullptr && (ext == ".cu8" || ext == ".cs16" || ext == ".cf32")) {
                // raw files carry no rate: still list them, with the length from the size
                const std::uint64_t fb = ext == ".cu8" ? 2 : (ext == ".cs16" ? 4 : 8);
                e.container = "raw"; e.format = ext.substr(1); e.totalFrames = e.sizeBytes / fb;
                const source::iqfile::NameGuess g = source::iqfile::guessFromFileName(e.path);
                if (g.hasFrequency && opt.guessRawFromName) { e.centerHz = g.frequencyHz; e.centerKnown = true; e.guessedFromName = true; }
                e.error = err;   // informational: why the rate is unknown
                return e;
            }
            if (r == nullptr) { e.broken = true; e.error = err; return e; }
            const source::iqfile::IqInfo& in = r->info();
            e.container = in.container; e.format = in.format; e.totalFrames = in.totalSamples; e.truncated = in.truncated; e.guessedFromName = in.guessedFromName;
            e.sampleRateHz = in.sampleRateHz; e.sampleRateKnown = in.sampleRateKnown;
            e.centerHz = in.centerFrequencyHz; e.centerKnown = in.centerKnown; e.channels = 2;
            if (in.sampleRateKnown && in.sampleRateHz > 0.0) { e.durationKnown = true; e.durationS = static_cast<double>(in.totalSamples) / in.sampleRateHz; }
            if (!in.captures.empty() && !in.captures[0].datetime.empty()) {
                std::int64_t ms = 0;
                if (parseIso8601Utc(in.captures[0].datetime, &ms)) { e.startUnixMs = ms; e.startSource = StartSource::SigmfDatetime; }
            }
        } else {
            e.broken = true; e.error = "unrecognised extension";
            return e;
        }
        if (e.kind == Kind::Iq && (e.container == "wav" || e.container == "rf64") && e.channels == 2) {
            // an I/Q-looking WAV: the I/Q reader may still refuse it (24-bit extensible with a strange GUID...): say so
            std::string err;
            source::iqfile::RawParams none;
            auto r = source::iqfile::openAuto(e.path, none, &err);
            if (r == nullptr) { e.kind = Kind::Audio; }   // not playable as I/Q: show it as audio
        }
        if (e.startSource == StartSource::None) {
            const NameTime nt = parseNameTime(e.fileName);
            if (nt.found) {
                e.startSource = nt.utc ? StartSource::FileNameUtc : StartSource::FileNameLocal;
                e.startUnixMs = nt.utc ? nt.unixMs : nt.unixMs - static_cast<std::int64_t>(opt.localUtcOffsetMinutes) * 60000;
            } else if (e.durationKnown && e.modifiedUnixMs != 0) {
                e.startSource = StartSource::ModifiedMinusDuration;
                e.startUnixMs = e.modifiedUnixMs - static_cast<std::int64_t>(e.durationS * 1000.0);
            }
        }
    } catch (const std::exception& ex) {
        e.broken = true; e.error = std::string("exception while reading the header: ") + ex.what();
    } catch (...) {
        e.broken = true; e.error = "unknown exception while reading the header";
    }
    return e;
}

inline bool isRecordingExtension(const std::string& lowerExt) {
    return lowerExt == ".wav" || lowerExt == ".rf64" || lowerExt == ".flac" || lowerExt == ".cu8" || lowerExt == ".cs16" || lowerExt == ".cf32" || lowerExt == ".sigmf-meta" ||
           lowerExt == ".sigmf-data";
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Sort and filter (pure).
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum class SortKey : int { Name = 0, Modified = 1, Start = 2, Duration = 3, Size = 4, Rate = 5, Center = 6, Kind = 7 };

struct Filter {
    std::string text;                      // case-insensitive substring of the file name; empty = all
    bool iq = true, audio = true;
    bool hideBroken = false;
    bool useCenter = false; double centerMinHz = 0.0, centerMaxHz = 0.0;           // entries without a known centre are excluded when set
    bool useDuration = false; double durationMinS = 0.0, durationMaxS = 0.0;       // entries without a known duration are excluded when set
    bool useStart = false; std::int64_t startFromUnixMs = 0, startToUnixMs = 0;    // inclusive; entries without a start are excluded when set
};

// Indices into `all`, filtered then stably sorted. Entries with an UNKNOWN value for the key go last in both directions.
inline std::vector<std::size_t> sortAndFilter(const std::vector<Entry>& all, const Filter& f, SortKey key, bool ascending) {
    std::vector<std::size_t> idx;
    const std::string needle = detail::lowerStr(f.text);
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Entry& e = all[i];
        if (e.kind == Kind::Iq && !f.iq) { continue; }
        if (e.kind == Kind::Audio && !f.audio) { continue; }
        if (e.broken && f.hideBroken) { continue; }
        if (!needle.empty() && detail::lowerStr(e.fileName).find(needle) == std::string::npos) { continue; }
        if (f.useCenter && !(e.centerKnown && e.centerHz >= f.centerMinHz && e.centerHz <= f.centerMaxHz)) { continue; }
        if (f.useDuration && !(e.durationKnown && e.durationS >= f.durationMinS && e.durationS <= f.durationMaxS)) { continue; }
        if (f.useStart && !(e.startSource != StartSource::None && e.startUnixMs >= f.startFromUnixMs && e.startUnixMs <= f.startToUnixMs)) { continue; }
        idx.push_back(i);
    }
    auto known = [&](const Entry& e) {
        switch (key) {
            case SortKey::Start: return e.startSource != StartSource::None;
            case SortKey::Duration: return e.durationKnown;
            case SortKey::Rate: return e.sampleRateKnown;
            case SortKey::Center: return e.centerKnown;
            default: return true;
        }
    };
    auto val = [&](const Entry& e) -> double {
        switch (key) {
            case SortKey::Modified: return static_cast<double>(e.modifiedUnixMs);
            case SortKey::Start: return static_cast<double>(e.startUnixMs);
            case SortKey::Duration: return e.durationS;
            case SortKey::Size: return static_cast<double>(e.sizeBytes);
            case SortKey::Rate: return e.sampleRateHz;
            case SortKey::Center: return e.centerHz;
            case SortKey::Kind: return static_cast<double>(static_cast<int>(e.kind));
            default: return 0.0;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        const Entry& x = all[a];
        const Entry& y = all[b];
        const bool kx = known(x), ky = known(y);
        if (kx != ky) { return kx; }                      // unknown last, whatever the direction
        if (!kx) { return false; }
        if (key == SortKey::Name) {
            const std::string nx = detail::lowerStr(x.fileName), ny = detail::lowerStr(y.fileName);
            return ascending ? nx < ny : nx > ny;
        }
        const double vx = val(x), vy = val(y);
        return ascending ? vx < vy : vx > vy;
    });
    return idx;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The scanner.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class RecordingsBrowser {
public:
    ~RecordingsBrowser() { cancel(); }

    // Starts a scan of `folder` (cancelling and joining a scan still running). Returns at once.
    void start(const std::string& folder, const ScanOptions& opt = {}) {
        cancel();
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_ = Snapshot{};
            snap_.scanId = ++scanId_;
        }
        stop_.store(false);
        running_.store(true);
        worker_ = std::thread([this, folder, opt] { run(folder, opt); });
    }

    void cancel() {
        stop_.store(true);
        if (worker_.joinable()) { worker_.join(); }
        running_.store(false);
    }

    // The latest published result: a copy of a few numbers and a shared pointer. Never waits for I/O.
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return snap_;
    }

    bool running() const { return running_.load(); }

private:
    void publish(std::vector<Entry>& list, std::size_t candidates, std::size_t probed, std::size_t ignored, std::size_t broken, bool done, bool cancelled, bool truncated,
                 const std::string& folderError) {
        auto copy = std::make_shared<const std::vector<Entry>>(list);
        std::lock_guard<std::mutex> lk(mu_);
        const std::uint64_t id = snap_.scanId;
        snap_.entries = std::move(copy);
        snap_.candidates = candidates; snap_.probed = probed; snap_.ignored = ignored; snap_.broken = broken;
        snap_.done = done; snap_.cancelled = cancelled; snap_.listTruncated = truncated; snap_.folderError = folderError; snap_.scanId = id;
    }

    void run(const std::string& folder, const ScanOptions& opt) {
        namespace fs = std::filesystem;
        std::vector<Entry> list;
        std::vector<fs::path> cand;
        std::size_t ignored = 0, brokenN = 0;
        bool truncated = false;
        std::string folderError;
        try {
            std::error_code ec;
            if (!fs::is_directory(fs::path(folder), ec)) {
                folderError = ec ? ("cannot open the folder: " + ec.message()) : "the folder does not exist or is not a folder";
                publish(list, 0, 0, 0, 0, true, false, false, folderError);
                running_.store(false);
                return;
            }
            // 1. list the candidates (cheap): recursion limited, links not followed, errors skipped
            std::vector<std::pair<fs::path, int>> stack{{fs::path(folder), 0}};
            while (!stack.empty() && !stop_.load()) {
                const auto [dir, depth] = stack.back();
                stack.pop_back();
                fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
                if (ec) { continue; }
                for (; !stop_.load() && it != fs::directory_iterator(); it.increment(ec)) {
                    if (ec) { break; }
                    std::error_code e2;
                    const fs::directory_entry& de = *it;
                    {   // links and junctions are never followed (a cycle would walk forever, a duplicate would list a recording twice)
                        const fs::file_type lt = de.symlink_status(e2).type();
                        if (lt == fs::file_type::symlink) { continue; }
#if defined(_MSC_VER)
                        if (lt == fs::file_type::junction) { continue; }
#endif
                    }
                    if (de.is_directory(e2)) { if (depth < opt.maxDepth) { stack.emplace_back(de.path(), depth + 1); } continue; }
                    if (!de.is_regular_file(e2)) { ++ignored; continue; }
                    const std::string ext = detail::lowerStr(de.path().extension().string());
                    if (!isRecordingExtension(ext)) { ++ignored; continue; }
                    if (cand.size() >= opt.maxEntries) { truncated = true; continue; }
                    cand.push_back(de.path());
                }
            }
            // a .sigmf-data next to its .sigmf-meta is the same recording: list the meta only
            {
                std::vector<fs::path> keep;
                keep.reserve(cand.size());
                for (const fs::path& p : cand) {
                    if (detail::lowerStr(p.extension().string()) == ".sigmf-data") {
                        fs::path meta = p; meta.replace_extension(".sigmf-meta");
                        std::error_code e3;
                        if (fs::exists(meta, e3)) { continue; }
                    }
                    keep.push_back(p);
                }
                cand.swap(keep);
            }
            publish(list, cand.size(), 0, ignored, 0, false, false, truncated, "");
            // 2. probe each
            auto lastPublish = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < cand.size() && !stop_.load(); ++i) {
                list.push_back(probeFile(cand[i], opt));
                if (list.back().broken) { ++brokenN; }
                const auto now = std::chrono::steady_clock::now();
                if (i % 256 == 255 || now - lastPublish > std::chrono::milliseconds(200)) {
                    publish(list, cand.size(), list.size(), ignored, brokenN, false, false, truncated, "");
                    lastPublish = now;
                }
            }
            publish(list, cand.size(), list.size(), ignored, brokenN, !stop_.load(), stop_.load(), truncated, "");
        } catch (const std::exception& ex) {
            publish(list, cand.size(), list.size(), ignored, brokenN, true, false, truncated, std::string("scan stopped: ") + ex.what());
        } catch (...) {
            publish(list, cand.size(), list.size(), ignored, brokenN, true, false, truncated, "scan stopped by an unknown error");
        }
        running_.store(false);
    }

    mutable std::mutex mu_;
    Snapshot snap_;
    std::uint64_t scanId_ = 0;
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false};
};

}  // namespace cascade::core::recbrowse
