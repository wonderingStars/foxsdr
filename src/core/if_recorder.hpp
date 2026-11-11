// IF (channel) recording: a writer for the I/Q of ONE channel at the channel sample rate, taken after the channel filter (what the receiver listens to,
// not the whole tuner band), with the header fields a later replay needs: sample rate, the RF frequency of the channel centre, mode and bandwidth
// (and the pass-band edges, the radio's name, the start time and the tuner's own centre when the caller has them).
//
// STANDALONE. Nothing here is wired (docs/feature-records/recording-models.md, Part 6). The existing recorder (core/recorder.cpp) writes the wide
// baseband I/Q of the whole tuner as float32 WAV, capped under 4 GiB; this file is the channel-rate counterpart, without the cap.
//
// FILE FORMAT. A WAV file with two channels (I, Q) that every I/Q reader of the previous batch opens as it is (IEEE float32, or PCM16), plus ONE private
// chunk "fxif" before the data chunk holding the metadata as a JSON object; a reader that does not know the chunk skips it (RIFF rule: unknown chunks
// are skipped, odd sizes padded). Layout: RIFF header, a reserved "JUNK" chunk of 28 bytes, "fmt ", "fxif", "data". When the data passes 4 GiB, finish()
// rewrites the header in place as RF64 per EBU Tech 3306 (the reserved chunk becomes "ds64", "RIFF" becomes "RF64", the 32-bit sizes become 0xFFFFFFFF
// and the 64-bit ones are in ds64), so a file of any length is valid and no rollover is needed (the rollover of core/rec_naming.hpp stays available).
//
// CRASH CONTRACT. open() writes a complete header declaring zero data and flushes. checkpoint() (the async recorder calls it every two seconds of wall
// time) rewrites the sizes for the data written so far, so a crash loses at most the last interval; before the first checkpoint the file is a valid
// zero-length recording (a reader that ignores the size field can still salvage the bytes).
//
// SAMPLE FORMATS: Float32 (bit exact, 8 bytes per frame) and Pcm16 (round(v * 32768) saturated, 4 bytes per frame; non-finite input is written as 0).
//
// THREADS. IfWriter: one thread, blocking file I/O. AsyncIfRecorder: push() from ONE producing thread, wait-free and allocation-free; one worker thread
// writes; a full ring drops and counts the frames that do not fit.
//
// SOURCES (clean room): RIFF/WAVE chunk layout and the format tags (Microsoft WAVEFORMATEX documentation); RF64 / ds64: EBU Tech 3306 v1.0
// (https://tech.ebu.ch/docs/tech/tech3306v1_0.pdf, read 2026-11-11 for the previous batch): "RF64" id, ds64 first, 64-bit riffSize / dataSize /
// sampleCount, 0xFFFFFFFF in the 32-bit fields. The JUNK-becomes-ds64 rewrite is the method that document describes for writers that cannot know the
// size in advance.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::core::ifrec {

using cf = std::complex<float>;

enum class SampleFormat : int { Float32 = 0, Pcm16 = 1 };

struct IfMeta {
    double sampleRateHz = 0.0;          // the channel rate; required, > 0
    double centerHz = 0.0;              // RF frequency of the channel centre (baseband 0 Hz); required finite
    std::string mode;                   // "USB", "NFM", ... free text up to 32 bytes
    double bandwidthHz = 0.0;           // the channel bandwidth (the filter), >= 0
    bool hasEdges = false;
    double lowCutHz = 0.0, highCutHz = 0.0;
    std::string radio;                  // up to 64 bytes
    std::int64_t startUnixMs = 0;       // 0 = unknown
    bool hasTunerCenter = false;
    double tunerCenterHz = 0.0;         // the radio's own centre when the channel is an offset inside it
};

struct IfHeader {
    bool ok = false;
    bool hasMeta = false;               // false for a plain WAV with no fxif chunk
    std::string error;
    IfMeta meta;
    SampleFormat format = SampleFormat::Float32;
    double sampleRateHz = 0.0;          // from the fmt chunk
    std::uint64_t dataOffset = 0;
    std::uint64_t frames = 0;
    bool rf64 = false;
    bool truncated = false;
};

namespace detail {

inline void le16(std::vector<std::uint8_t>& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }
inline void le32(std::vector<std::uint8_t>& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); } }
inline void le64(std::vector<std::uint8_t>& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) { b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); } }
inline void tag(std::vector<std::uint8_t>& b, const char* s) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(s[i])); } }
inline std::uint32_t rd32(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
inline std::uint64_t rd64(const unsigned char* p) { return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32); }

inline std::string cut(const std::string& s, std::size_t n) {
    if (s.size() <= n) { return s; }
    std::size_t k = n;
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) { --k; }
    return s.substr(0, k);
}

inline std::string metaJson(const IfMeta& m) {
    nlohmann::json j;
    j["foxsdr_if"] = 1;
    j["sample_rate_hz"] = m.sampleRateHz;
    j["center_hz"] = m.centerHz;
    j["mode"] = cut(m.mode, 32);
    j["bandwidth_hz"] = m.bandwidthHz;
    if (m.hasEdges) { j["low_cut_hz"] = m.lowCutHz; j["high_cut_hz"] = m.highCutHz; }
    if (!m.radio.empty()) { j["radio"] = cut(m.radio, 64); }
    if (m.startUnixMs != 0) { j["start_unix_ms"] = m.startUnixMs; }
    if (m.hasTunerCenter) { j["tuner_center_hz"] = m.tunerCenterHz; }
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace detail

// Checks the metadata a writer is about to record. Empty string when fine, else the reason.
inline std::string validateMeta(const IfMeta& m) {
    if (!std::isfinite(m.sampleRateHz) || m.sampleRateHz <= 0.0 || m.sampleRateHz > 1e9) { return "the sample rate must be between 0 and 1 GHz"; }
    if (!std::isfinite(m.centerHz) || m.centerHz < 0.0 || m.centerHz > 1e13) { return "the centre frequency is not a valid frequency"; }
    if (!std::isfinite(m.bandwidthHz) || m.bandwidthHz < 0.0) { return "the bandwidth must be zero or positive"; }
    if (m.hasEdges && (!std::isfinite(m.lowCutHz) || !std::isfinite(m.highCutHz))) { return "the pass-band edges must be finite"; }
    if (m.hasTunerCenter && (!std::isfinite(m.tunerCenterHz) || m.tunerCenterHz < 0.0)) { return "the tuner centre is not a valid frequency"; }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Writer.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class IfWriter {
public:
    ~IfWriter() { if (open_) { finish(nullptr); } }

    bool open(const std::string& path, const IfMeta& meta, SampleFormat fmt, std::string* err) {
        if (open_) { return fail(err, "already open"); }
        const std::string why = validateMeta(meta);
        if (!why.empty()) { return fail(err, why); }
        fmt_ = fmt;
        frameBytes_ = fmt == SampleFormat::Float32 ? 8 : 4;
        rate_ = static_cast<std::uint32_t>(std::min(meta.sampleRateHz, 4294967295.0) + 0.5);
        file_.clear();
        file_.open(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!file_) { return fail(err, "cannot create " + path); }
        std::string js = detail::metaJson(meta);
        std::vector<std::uint8_t> h;
        detail::tag(h, "RIFF"); detail::le32(h, 0); detail::tag(h, "WAVE");
        detail::tag(h, "JUNK"); detail::le32(h, 28); h.resize(h.size() + 28, 0);                // becomes ds64 when the file passes 4 GiB
        detail::tag(h, "fmt ");
        detail::le32(h, 18); detail::le16(h, fmt == SampleFormat::Float32 ? 3 : 1); detail::le16(h, 2); detail::le32(h, rate_);
        detail::le32(h, rate_ * frameBytes_); detail::le16(h, frameBytes_); detail::le16(h, fmt == SampleFormat::Float32 ? 32 : 16); detail::le16(h, 0);
        detail::tag(h, "fxif"); detail::le32(h, static_cast<std::uint32_t>(js.size()));
        h.insert(h.end(), js.begin(), js.end());
        if ((js.size() & 1) != 0) { h.push_back(0); }
        detail::tag(h, "data"); detail::le32(h, 0);
        dataOffset_ = h.size();
        file_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
        file_.flush();
        if (!file_) { file_.close(); return fail(err, "write failed (disk full?)"); }
        headerSize_ = h.size();
        dataBytes_ = 0; failed_ = false; error_.clear();
        open_ = true;
        return true;
    }

    // Writes n complex frames. False on a write failure (then every later call fails too; finish() still closes the file).
    bool write(const cf* x, std::size_t n, std::string* err = nullptr) {
        if (!open_) { return fail(err, "not open"); }
        if (failed_) { return fail(err, error_); }
        std::size_t done = 0;
        while (done < n) {
            const std::size_t chunk = std::min<std::size_t>(n - done, 8192);
            buf_.resize(chunk * frameBytes_);
            std::uint8_t* p = buf_.data();
            if (fmt_ == SampleFormat::Float32) {
                for (std::size_t i = 0; i < chunk; ++i) {
                    const float re = x[done + i].real(), im = x[done + i].imag();
                    std::memcpy(p, &re, 4); std::memcpy(p + 4, &im, 4); p += 8;
                }
            } else {
                for (std::size_t i = 0; i < chunk; ++i) {
                    const float c[2] = {x[done + i].real(), x[done + i].imag()};
                    for (int k = 0; k < 2; ++k) {
                        const double v = std::isfinite(c[k]) ? static_cast<double>(c[k]) * 32768.0 : 0.0;
                        const long q = std::clamp(std::lround(v), -32768L, 32767L);
                        const std::int16_t s = static_cast<std::int16_t>(q);
                        std::memcpy(p, &s, 2); p += 2;
                    }
                }
            }
            file_.write(reinterpret_cast<const char*>(buf_.data()), static_cast<std::streamsize>(chunk * frameBytes_));
            if (!file_) { failed_ = true; error_ = "write failed (disk full or file removed)"; return fail(err, error_); }
            dataBytes_ += chunk * frameBytes_;
            done += chunk;
        }
        file_.flush();   // what a crash leaves behind is at most the last write call (and, with the header sizes, the last checkpoint interval)
        if (!file_) { failed_ = true; error_ = "write failed (disk full or file removed)"; return fail(err, error_); }
        return true;
    }

    // Rewrites the header sizes for the data written so far and flushes (the file stays open for more data).
    bool checkpoint(std::string* err = nullptr) {
        if (!open_) { return fail(err, "not open"); }
        if (failed_) { return fail(err, error_); }
        if (!patchHeader(false)) { return fail(err, error_); }
        return true;
    }

    // Final header (RF64 when the data passes the 32-bit limit), flush, close. Idempotent.
    bool finish(std::string* err = nullptr) {
        if (!open_) { return true; }
        bool ok = !failed_;
        if (ok) { ok = patchHeader(true); }
        file_.flush();
        file_.close();
        open_ = false;
        if (!ok) { return fail(err, error_); }
        return true;
    }

    bool isOpen() const { return open_; }
    std::uint64_t framesWritten() const { return dataBytes_ / frameBytes_; }
    std::uint64_t bytesWritten() const { return headerSize_ + dataBytes_; }
    std::uint64_t dataOffset() const { return dataOffset_; }
    const std::string& lastError() const { return error_; }

    // THE LIMIT: a plain RIFF holds up to 4 GiB - 8 bytes in its size fields; above it the header is rewritten as RF64.
    static constexpr std::uint64_t kRiffLimit = 0xFFFFFFFFull - 64;

    // Rewrites a finished file's header for a data size that the file does not actually hold (for the over-4 GB tests: a sparse file is extended to the size
    // afterwards). Returns false when the file cannot be patched.
    static bool patchSizesForTest(const std::string& path, std::uint64_t claimedDataBytes) {
        std::fstream f(std::filesystem::path(path), std::ios::binary | std::ios::in | std::ios::out);
        if (!f) { return false; }
        std::vector<std::uint8_t> head(8192);
        f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(f.gcount()));
        f.clear();   // the short read set eofbit / failbit
        std::size_t dataAt = 0;
        for (std::size_t i = 12; i + 8 <= head.size();) {
            const std::uint32_t sz = detail::rd32(head.data() + i + 4);
            if (std::memcmp(head.data() + i, "data", 4) == 0) { dataAt = i; break; }
            i += 8 + sz + (sz & 1);
        }
        if (dataAt == 0) { return false; }
        return writeSizes(f, dataAt + 8, dataAt, claimedDataBytes, claimedDataBytes / 8, true);
    }

private:
    static bool fail(std::string* err, const std::string& m) { if (err != nullptr) { *err = m; } return false; }

    // Writes the size fields. `dataAt` is the offset of the "data" chunk id; the header ends at dataAt + 8.
    static bool writeSizes(std::fstream& f, std::uint64_t headerSize, std::uint64_t dataAt, std::uint64_t dataBytes, std::uint64_t frames, bool allowRf64) {
        const std::uint64_t riffSize64 = headerSize - 8 + dataBytes + (dataBytes & 1);
        const bool big = allowRf64 && (riffSize64 > kRiffLimit || dataBytes > kRiffLimit);
        f.seekp(0);
        std::vector<std::uint8_t> b;
        if (big) {
            detail::tag(b, "RF64"); detail::le32(b, 0xFFFFFFFFu); detail::tag(b, "WAVE");
            detail::tag(b, "ds64"); detail::le32(b, 28); detail::le64(b, riffSize64); detail::le64(b, dataBytes); detail::le64(b, frames); detail::le32(b, 0);
            f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
            f.seekp(static_cast<std::streamoff>(dataAt + 4));
            std::vector<std::uint8_t> d; detail::le32(d, 0xFFFFFFFFu);
            f.write(reinterpret_cast<const char*>(d.data()), 4);
        } else {
            detail::tag(b, "RIFF"); detail::le32(b, static_cast<std::uint32_t>(std::min<std::uint64_t>(riffSize64, 0xFFFFFFFFull))); detail::tag(b, "WAVE");
            detail::tag(b, "JUNK"); detail::le32(b, 28); b.resize(b.size() + 28, 0);
            f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
            f.seekp(static_cast<std::streamoff>(dataAt + 4));
            std::vector<std::uint8_t> d; detail::le32(d, static_cast<std::uint32_t>(std::min<std::uint64_t>(dataBytes, 0xFFFFFFFFull)));
            f.write(reinterpret_cast<const char*>(d.data()), 4);
        }
        f.flush();
        return static_cast<bool>(f);
    }

    bool patchHeader(bool final) {
        const std::uint64_t dataAt = dataOffset_ - 8;
        if (!final && dataBytes_ > kRiffLimit) { return true; }   // beyond 4 GiB a checkpoint cannot be expressed in plain RIFF: the file is repaired at finish()
        if (!writeSizes(file_, dataOffset_, dataAt, dataBytes_, dataBytes_ / frameBytes_, final)) { failed_ = true; error_ = "header update failed"; return false; }
        file_.seekp(0, std::ios::end);
        return true;
    }

    std::fstream file_;
    bool open_ = false, failed_ = false;
    SampleFormat fmt_ = SampleFormat::Float32;
    unsigned frameBytes_ = 8;
    std::uint32_t rate_ = 0;
    std::uint64_t dataOffset_ = 0, headerSize_ = 0, dataBytes_ = 0;
    std::string error_;
    std::vector<std::uint8_t> buf_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Reader of the header (bounded; never throws).
// ---------------------------------------------------------------------------------------------------------------------------------------------
inline IfHeader readIfHeader(const std::string& path) {
    IfHeader h;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) { h.error = "cannot open the file"; return h; }
    std::error_code ec;
    const std::uint64_t fileSize = std::filesystem::file_size(std::filesystem::path(path), ec);
    unsigned char hd[12];
    f.read(reinterpret_cast<char*>(hd), 12);
    if (f.gcount() != 12) { h.error = "shorter than a WAV header"; return h; }
    h.rf64 = std::memcmp(hd, "RF64", 4) == 0 || std::memcmp(hd, "BW64", 4) == 0;
    if ((!h.rf64 && std::memcmp(hd, "RIFF", 4) != 0) || std::memcmp(hd + 8, "WAVE", 4) != 0) { h.error = "not a WAVE file"; return h; }
    std::uint64_t pos = 12, ds64Data = 0;
    bool haveFmt = false, haveDs64 = false;
    unsigned tagv = 0, blockAlign = 0, bits = 0, channels = 0;
    for (int chunk = 0; chunk < 64; ++chunk) {
        unsigned char ch[8];
        f.clear(); f.seekg(static_cast<std::streamoff>(pos));
        f.read(reinterpret_cast<char*>(ch), 8);
        if (f.gcount() != 8) { break; }
        const std::uint64_t size = detail::rd32(ch + 4);
        const std::uint64_t body = pos + 8;
        if (std::memcmp(ch, "ds64", 4) == 0 && size >= 16) {
            unsigned char d[16];
            f.read(reinterpret_cast<char*>(d), 16);
            if (f.gcount() == 16) { ds64Data = detail::rd64(d + 8); haveDs64 = true; }
        } else if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            unsigned char fm[16];
            f.read(reinterpret_cast<char*>(fm), 16);
            if (f.gcount() != 16) { h.error = "fmt chunk cut short"; return h; }
            tagv = fm[0] | (fm[1] << 8); channels = fm[2] | (fm[3] << 8); h.sampleRateHz = detail::rd32(fm + 4); blockAlign = fm[12] | (fm[13] << 8); bits = fm[14] | (fm[15] << 8);
            haveFmt = true;
        } else if (std::memcmp(ch, "fxif", 4) == 0) {
            if (size == 0 || size > 4096) { h.error = "the fxif chunk has an implausible size"; return h; }
            std::string js(static_cast<std::size_t>(size), '\0');
            f.read(js.data(), static_cast<std::streamsize>(size));
            if (static_cast<std::uint64_t>(f.gcount()) != size) { h.error = "the fxif chunk is cut short"; return h; }
            const nlohmann::json j = nlohmann::json::parse(js, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("foxsdr_if")) { h.error = "the fxif chunk is not FoxSDR IF metadata"; return h; }
            auto num = [&](const char* k, double* out) { if (j.contains(k) && j[k].is_number()) { *out = j[k].get<double>(); return true; } return false; };
            IfMeta m;
            if (!num("sample_rate_hz", &m.sampleRateHz) || !num("center_hz", &m.centerHz)) { h.error = "the fxif metadata lacks the sample rate or the centre frequency"; return h; }
            num("bandwidth_hz", &m.bandwidthHz);
            if (j.contains("mode") && j["mode"].is_string()) { m.mode = j["mode"].get<std::string>(); }
            if (j.contains("radio") && j["radio"].is_string()) { m.radio = j["radio"].get<std::string>(); }
            if (num("low_cut_hz", &m.lowCutHz) && num("high_cut_hz", &m.highCutHz)) { m.hasEdges = true; }
            if (j.contains("start_unix_ms") && j["start_unix_ms"].is_number_integer()) { m.startUnixMs = j["start_unix_ms"].get<std::int64_t>(); }
            if (num("tuner_center_hz", &m.tunerCenterHz)) { m.hasTunerCenter = true; }
            if (!validateMeta(m).empty()) { h.error = "the fxif metadata is invalid: " + validateMeta(m); return h; }
            h.meta = m; h.hasMeta = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            if (!haveFmt) { h.error = "data chunk before the fmt chunk"; return h; }
            if (channels != 2 || !((tagv == 3 && bits == 32) || (tagv == 1 && bits == 16))) { h.error = "not a two-channel float32 or PCM16 file"; return h; }
            h.format = tagv == 3 ? SampleFormat::Float32 : SampleFormat::Pcm16;
            const std::uint64_t avail = fileSize > body ? fileSize - body : 0;
            std::uint64_t dsize = size;
            if (size == 0xFFFFFFFFull && h.rf64 && haveDs64) { dsize = ds64Data; }
            else if (size == 0xFFFFFFFFull || size == 0) { dsize = avail; }
            if (dsize > avail) { dsize = avail; h.truncated = true; }
            h.dataOffset = body;
            h.frames = blockAlign != 0 ? dsize / blockAlign : 0;
            h.ok = true;
            return h;
        }
        pos = body + size + (size & 1);
        if (pos >= fileSize) { break; }
    }
    h.error = "no data chunk";
    return h;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Async recorder.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class AsyncIfRecorder {
public:
    ~AsyncIfRecorder() { stop(nullptr); }

    bool start(const std::string& path, const IfMeta& meta, SampleFormat fmt, std::string* err, std::size_t ringFrames = 1u << 18, int checkpointMs = 2000) {
        if (running_.load()) { if (err != nullptr) { *err = "already running"; } return false; }
        if (!writer_.open(path, meta, fmt, err)) { return false; }
        cap_ = 1; while (cap_ < ringFrames) { cap_ <<= 1; }
        ring_.assign(cap_, cf(0.0f, 0.0f));
        head_.store(0); tail_.store(0); dropped_.store(0); accepted_.store(0); failed_.store(false);
        lastError_.clear();
        checkpointMs_ = std::max(50, checkpointMs);
        stopFlag_.store(false); running_.store(true);
        worker_ = std::thread([this] { run(); });
        return true;
    }

    // Producer thread only: wait-free, allocation-free. Returns the frames accepted; the rest are counted as dropped.
    std::size_t push(const cf* x, std::size_t n) {
        if (!running_.load(std::memory_order_relaxed)) { return 0; }
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        const std::size_t fit = std::min(n, cap_ - static_cast<std::size_t>(h - t));
        for (std::size_t i = 0; i < fit; ++i) { ring_[static_cast<std::size_t>((h + i) & (cap_ - 1))] = x[i]; }
        head_.store(h + fit, std::memory_order_release);
        accepted_.fetch_add(fit, std::memory_order_relaxed);
        if (fit < n) { dropped_.fetch_add(n - fit, std::memory_order_relaxed); }
        return fit;
    }

    bool stop(std::string* err = nullptr) {
        if (!running_.load()) { return true; }
        stopFlag_.store(true);
        if (worker_.joinable()) { worker_.join(); }
        running_.store(false);
        std::string e;
        const bool fin = writer_.finish(&e);
        const bool ok = fin && !failed_.load();
        if (!ok && err != nullptr) { *err = lastError_.empty() ? e : lastError_; }
        return ok;
    }

    bool running() const { return running_.load(); }
    std::uint64_t droppedFrames() const { return dropped_.load(); }
    std::uint64_t acceptedFrames() const { return accepted_.load(); }
    bool failed() const { return failed_.load(); }
    int checkpointsWritten() const { return checkpoints_.load(); }

private:
    void run() {
        std::vector<cf> q(4096);
        auto lastCp = std::chrono::steady_clock::now();
        for (;;) {
            const bool stopping = stopFlag_.load();
            const std::uint64_t h = head_.load(std::memory_order_acquire);
            std::uint64_t t = tail_.load(std::memory_order_relaxed);
            bool did = false;
            while (h > t) {
                const std::size_t n = std::min<std::size_t>(q.size(), static_cast<std::size_t>(h - t));
                for (std::size_t i = 0; i < n; ++i) { q[i] = ring_[static_cast<std::size_t>((t + i) & (cap_ - 1))]; }
                t += n;
                tail_.store(t, std::memory_order_release);
                std::string e;
                if (!failed_.load() && !writer_.write(q.data(), n, &e)) { lastError_ = e; failed_.store(true); }
                did = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!failed_.load() && now - lastCp >= std::chrono::milliseconds(checkpointMs_)) {
                std::string e;
                if (writer_.checkpoint(&e)) { ++checkpoints_; } else { lastError_ = e; failed_.store(true); }
                lastCp = now;
            }
            if (stopping && !did) { return; }
            if (!did) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        }
    }

    IfWriter writer_;
    std::thread worker_;
    std::vector<cf> ring_;
    std::size_t cap_ = 0;
    int checkpointMs_ = 2000;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0}, accepted_{0};
    std::atomic<bool> running_{false}, stopFlag_{false}, failed_{false};
    std::atomic<int> checkpoints_{0};
    std::string lastError_;
};

}  // namespace cascade::core::ifrec
