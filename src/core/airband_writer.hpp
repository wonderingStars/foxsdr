// airband_writer.hpp - the airband monitor's PER-CHANNEL OUTPUTS: a WAV file per channel (in a folder per UTC day, with a size cap) and a UDP stream to
// 127.0.0.1. Both OFF until the user switches one on for a channel. Not wired (hand-off in docs/feature-records/dspmodels.md).
//
// POLICY (unchanged): the airband monitor already listens to other people's voice; this adds a way to KEEP or FORWARD what it plays, so every output
// is off by default, per channel, and the file only ever gets what the squelch let through.
//
// FILE OUTPUT (ChannelWavWriter).
//   - WHERE: <recordingsRoot>/airband/<YYYY-MM-DD>/ (UTC; or <recordingsRoot>/airband/ with the date in the name when dailyFolder is off). The root is
//     the recordings root the host already has and must EXIST and be a directory; this writer creates only the two levels below it. Every part of
//     the path is built from digits and fixed words (never from text the user or a signal supplied), and pathIsUnder() is checked before anything is
//     opened: a path that is not under the root is refused.
//   - NAME: ch<NN>_<kHz>kHz_<segment 0001..>.wav (with _<YYYYMMDD> after the frequency when there is no day folder). The channel NUMBER and its
//     frequency in kilohertz, nothing else: no call sign, no position, no name.
//   - FORMAT: 16-bit PCM, mono, the channel's sample rate (48000), 44-byte header. The header's sizes are written when the file is opened and
//     PATCHED after every burst and on close, so the file is a valid WAV even if the program dies between bursts.
//   - WHAT: only what the squelch opened for: it is given the audio from BEFORE the gate plus the gate's state; when the gate opens the last preRollMs
//     of audio go first (so the first syllable is not cut), then everything until tailMs after it closes; between bursts nothing is written, and the
//     next burst continues the same file.
//   - SIZE CAP: maxFileBytes of audio data per file (1 MB .. 2000 MB); the burst that would pass it continues in the next segment (_0002 ...).
//     A new UTC day always starts a new file in the new folder.
//   - ERRORS: a failed open or write never throws and never blocks the caller: the writer goes to the Failed state, drops what it is given, counts
//     it, keeps `lastError` (no path in it) and tries again only after reset().
//   - THREAD: file I/O blocks, so call write() from a recorder thread, never from the audio thread.
//
// STREAM OUTPUT (LoopbackAudioSink): the channel's gated audio as 16-bit mono datagrams (net::UdpAudioSender's wire format) to 127.0.0.1 and a port
// the user chose (1024 .. 65535). The destination is NOT a parameter: there is no way to point it at another machine from here. It only sends; it
// never reads from the network; a full ring drops audio and counts it. Silence is not sent. start() is the only thing that opens a socket.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "net/udp_audio.hpp"

namespace cascade::core::airband {

struct UtcStamp {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    bool operator==(const UtcStamp&) const = default;
};

inline bool stampValid(const UtcStamp& t) {
    return t.year >= 2000 && t.year <= 2999 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour >= 0 && t.hour <= 23 && t.minute >= 0 &&
           t.minute <= 59 && t.second >= 0 && t.second <= 60;
}

inline std::string two(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", v);
    return b;
}
inline std::string dayFolderName(const UtcStamp& t) {
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", t.year, t.month, t.day);
    return b;
}
inline std::string dayCompact(const UtcStamp& t) {
    char b[16];
    std::snprintf(b, sizeof b, "%04d%02d%02d", t.year, t.month, t.day);
    return b;
}
// ch03_118700kHz_0001.wav  (or ch03_118700kHz_20261112_0001.wav without a day folder). Frequency rounded to whole kilohertz.
inline std::string fileName(int channelIndex, double freqHz, const UtcStamp& day, int segment, bool dailyFolder) {
    const long long khz = std::isfinite(freqHz) && freqHz > 0.0 ? static_cast<long long>(std::llround(freqHz / 1000.0)) : 0;
    char b[96];
    const int idx = std::min(std::max(channelIndex, 0), 99);
    const int seg = std::min(std::max(segment, 1), 9999);
    if (dailyFolder) { std::snprintf(b, sizeof b, "ch%02d_%lldkHz_%04d.wav", idx, khz, seg); }
    else { std::snprintf(b, sizeof b, "ch%02d_%lldkHz_%s_%04d.wav", idx, khz, dayCompact(day).c_str(), seg); }
    return b;
}

// True when `path` is `root` or lies inside it (after making both absolute and normal; symbolic links are not followed, because the writer creates
// every folder under the root itself).
inline bool pathIsUnder(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path r = std::filesystem::absolute(root, ec).lexically_normal();
    const std::filesystem::path p = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec || r.empty()) { return false; }
    auto ri = r.begin();
    auto pi = p.begin();
    for (; ri != r.end(); ++ri, ++pi) {
        if (ri->empty()) { continue; }  // a trailing separator
        if (pi == p.end() || *ri != *pi) { return false; }
    }
    for (; pi != p.end(); ++pi) {
        if (*pi == "..") { return false; }
    }
    return true;
}

struct WriterConfig {
    std::string recordingsRoot;
    int channelIndex = 0;
    double freqHz = 0.0;
    bool dailyFolder = true;
    std::uint64_t maxFileBytes = 100ull * 1024ull * 1024ull;
    int sampleRate = 48000;
    double preRollMs = 200.0;
    double tailMs = 300.0;
};

enum class WriterState : int { Idle = 0, Writing = 1, Failed = 2 };

class ChannelWavWriter {
public:
    static constexpr std::uint64_t kMinFileBytes = 1ull << 20;
    static constexpr std::uint64_t kMaxFileBytes = 2000ull << 20;

    explicit ChannelWavWriter(const WriterConfig& c = WriterConfig{}) { configure(c); }
    ~ChannelWavWriter() { closeFile(); }
    ChannelWavWriter(const ChannelWavWriter&) = delete;
    ChannelWavWriter& operator=(const ChannelWavWriter&) = delete;

    void configure(const WriterConfig& c) {
        closeFile();
        cfg_ = c;
        cfg_.sampleRate = std::min(std::max(cfg_.sampleRate, 8000), 192000);
        cfg_.maxFileBytes = std::min(std::max(cfg_.maxFileBytes, kMinFileBytes), kMaxFileBytes);
        cfg_.preRollMs = std::isfinite(cfg_.preRollMs) ? std::min(std::max(cfg_.preRollMs, 0.0), 2000.0) : 200.0;
        cfg_.tailMs = std::isfinite(cfg_.tailMs) ? std::min(std::max(cfg_.tailMs, 0.0), 5000.0) : 300.0;
        preCap_ = static_cast<std::size_t>(cfg_.sampleRate * cfg_.preRollMs / 1000.0);
        tailSamples_ = static_cast<std::size_t>(cfg_.sampleRate * cfg_.tailMs / 1000.0);
        pre_.assign(std::max<std::size_t>(preCap_, 1), 0.0f);
        preFill_ = preHead_ = 0;
        state_ = WriterState::Idle;
        segment_ = 0;
        tailLeft_ = 0;
        lastError_.clear();
    }
    const WriterConfig& config() const { return cfg_; }

    WriterState state() const { return state_; }
    const std::string& lastError() const { return lastError_; }
    std::uint64_t bytesWritten() const { return totalBytes_; }
    std::uint64_t filesOpened() const { return files_; }
    std::uint64_t droppedSamples() const { return dropped_; }
    bool fileOpen() const { return out_.is_open(); }
    // The file being written (empty when none): for the node's status line and for the tests.
    const std::filesystem::path& currentPath() const { return path_; }

    // Forgets a failure and starts clean (a file open stays closed; the next burst opens the next segment).
    void reset() {
        closeFile();
        state_ = WriterState::Idle;
        lastError_.clear();
        preFill_ = preHead_ = 0;
        tailLeft_ = 0;
    }

    // Hands the writer one block of the channel's audio taken BEFORE the squelch gate (ChannelProcessor::process hands it out as `preGate`: a gate
    // that has zeroed the audio would leave the pre-roll and the tail empty) and whether the gate is open at the end of the block. `now` is the
    // caller's UTC clock. Returns true when the block was accepted (written, or kept as pre-roll, or let go because nothing is being recorded);
    // false when the writer is Failed.
    bool write(const float* audio, std::size_t n, bool gateOpen, const UtcStamp& now) {
        if (audio == nullptr || n == 0) { return state_ != WriterState::Failed; }
        if (state_ == WriterState::Failed) {
            dropped_ += n;
            return false;
        }
        if (!stampValid(now)) { return fail("The clock reading is not usable.", n); }
        // A new UTC day closes the file: the next burst opens a file in the new day's folder.
        if (out_.is_open() && !(now.year == day_.year && now.month == day_.month && now.day == day_.day)) {
            finishBurst();
            closeFile();
            segment_ = 0;
        }
        if (gateOpen) {
            if (tailLeft_ == 0 && !recording_) {
                // the gate has just opened: the pre-roll goes first
                if (!openIfNeeded(now)) { return false; }
                if (!flushPreRoll()) { return false; }
                recording_ = true;
            }
            tailLeft_ = tailSamples_;
            recording_ = true;
            return emit(audio, n, now);
        }
        // closed: the tail, then hold the audio as pre-roll for the next burst
        if (recording_) {
            const std::size_t take = std::min(n, tailLeft_);
            if (take > 0 && !emit(audio, take, now)) { return false; }
            tailLeft_ -= take;
            if (tailLeft_ == 0) {
                recording_ = false;
                finishBurst();
                pushPreRoll(audio + take, n - take);
            }
            return true;
        }
        pushPreRoll(audio, n);
        return true;
    }

    // Ends the burst in progress and closes the file (the header is final).
    void close() {
        if (out_.is_open()) { finishBurst(); }
        closeFile();
        recording_ = false;
        tailLeft_ = 0;
    }

private:
    bool fail(const char* why, std::size_t n) {
        state_ = WriterState::Failed;
        lastError_ = why;
        dropped_ += n;
        closeFile();
        recording_ = false;
        return false;
    }

    bool openIfNeeded(const UtcStamp& now) {
        if (out_.is_open()) { return true; }
        if (cfg_.recordingsRoot.empty()) { return fail("No recordings folder is set.", 0); }
        std::error_code ec;
        const std::filesystem::path root(cfg_.recordingsRoot);
        if (!std::filesystem::is_directory(root, ec)) { return fail("The recordings folder does not exist.", 0); }
        std::filesystem::path dir = root / "airband";
        if (cfg_.dailyFolder) { dir /= dayFolderName(now); }
        if (!pathIsUnder(root, dir)) { return fail("The output folder is not under the recordings folder.", 0); }
        std::filesystem::create_directories(dir, ec);
        if (ec) { return fail("The output folder could not be created.", 0); }
        ++segment_;
        const std::filesystem::path file = dir / fileName(cfg_.channelIndex, cfg_.freqHz, now, segment_, cfg_.dailyFolder);
        if (!pathIsUnder(root, file)) { return fail("The output file is not under the recordings folder.", 0); }
        out_.open(file, std::ios::binary | std::ios::trunc);
        if (!out_) { return fail("The output file could not be opened.", 0); }
        path_ = file;
        day_ = now;
        dataBytes_ = 0;
        writeHeader();
        if (!out_) { return fail("The output file could not be written.", 0); }
        ++files_;
        state_ = WriterState::Writing;
        return true;
    }

    // Pre-roll ring: the most recent preCap_ samples while the gate is closed.
    void pushPreRoll(const float* a, std::size_t n) {
        if (preCap_ == 0) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            pre_[preHead_] = a[i];
            preHead_ = (preHead_ + 1) % preCap_;
            if (preFill_ < preCap_) { ++preFill_; }
        }
    }
    bool flushPreRoll() {
        if (preFill_ == 0) { return true; }
        std::vector<float> tmp(preFill_);
        const std::size_t start = (preHead_ + preCap_ - preFill_) % preCap_;
        for (std::size_t i = 0; i < preFill_; ++i) { tmp[i] = pre_[(start + i) % preCap_]; }
        preFill_ = 0;
        return emit(tmp.data(), tmp.size(), day_);
    }

    // Writes samples, splitting into segments at the size cap.
    bool emit(const float* a, std::size_t n, const UtcStamp& now) {
        std::size_t done = 0;
        while (done < n) {
            if (!out_.is_open()) { if (!openIfNeeded(now)) { return false; } }
            const std::uint64_t room = cfg_.maxFileBytes - dataBytes_;
            if (room < 2) {
                finishBurst();
                closeFile();  // the next iteration opens the next segment
                continue;
            }
            const std::size_t can = static_cast<std::size_t>(std::min<std::uint64_t>(room / 2, n - done));
            buf_.resize(can * 2);
            for (std::size_t i = 0; i < can; ++i) {
                float v = std::isfinite(a[done + i]) ? a[done + i] : 0.0f;
                v = std::min(std::max(v, -1.0f), 1.0f);
                const std::int16_t s = static_cast<std::int16_t>(std::lrintf(v * 32767.0f));
                buf_[2 * i] = static_cast<char>(s & 0xFF);
                buf_[2 * i + 1] = static_cast<char>((s >> 8) & 0xFF);
            }
            out_.write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
            if (!out_) { return fail("Writing to the output file failed.", n - done); }
            dataBytes_ += buf_.size();
            totalBytes_ += buf_.size();
            done += can;
        }
        out_.flush();  // what was accepted is on disk when the call returns (the header's sizes follow at the end of the burst)
        if (!out_) { return fail("Writing to the output file failed.", 0); }
        return true;
    }

    void writeHeader() {
        unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
                               'd', 'a', 't', 'a', 0, 0, 0, 0};
        put32(h + 24, static_cast<std::uint32_t>(cfg_.sampleRate));
        put32(h + 28, static_cast<std::uint32_t>(cfg_.sampleRate) * 2u);
        put32(h + 4, static_cast<std::uint32_t>(36u + dataBytes_));
        put32(h + 40, static_cast<std::uint32_t>(dataBytes_));
        out_.write(reinterpret_cast<const char*>(h), 44);
    }
    static void put32(unsigned char* p, std::uint32_t v) {
        p[0] = static_cast<unsigned char>(v & 0xFF);
        p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
        p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
    }
    // Patches the two size fields to what has been written so far and returns to the end.
    void finishBurst() {
        if (!out_.is_open()) { return; }
        unsigned char a[4], b[4];
        put32(a, static_cast<std::uint32_t>(36u + dataBytes_));
        put32(b, static_cast<std::uint32_t>(dataBytes_));
        out_.seekp(4);
        out_.write(reinterpret_cast<const char*>(a), 4);
        out_.seekp(40);
        out_.write(reinterpret_cast<const char*>(b), 4);
        out_.seekp(0, std::ios::end);
        out_.flush();
        if (!out_) { state_ = WriterState::Failed; lastError_ = "Writing to the output file failed."; }
    }
    void closeFile() {
        if (out_.is_open()) {
            out_.close();
        }
        out_.clear();
    }

    WriterConfig cfg_;
    WriterState state_ = WriterState::Idle;
    std::string lastError_;
    std::ofstream out_;
    std::filesystem::path path_;
    UtcStamp day_;
    int segment_ = 0;
    std::uint64_t dataBytes_ = 0, totalBytes_ = 0, files_ = 0, dropped_ = 0;
    bool recording_ = false;
    std::size_t tailLeft_ = 0, tailSamples_ = 0, preCap_ = 0, preFill_ = 0, preHead_ = 0;
    std::vector<float> pre_;
    std::vector<char> buf_;
};

// ------------------------------------------------------------------ stream

class LoopbackAudioSink {
public:
    static constexpr int kMinPort = 1024, kMaxPort = 65535;
    static constexpr std::size_t kRingFrames = 1u << 16;  // about 1.4 s at 48 kHz

    LoopbackAudioSink() : ring_(kRingFrames) {}
    ~LoopbackAudioSink() { stop(); }
    LoopbackAudioSink(const LoopbackAudioSink&) = delete;
    LoopbackAudioSink& operator=(const LoopbackAudioSink&) = delete;

    // Opens the socket and starts sending (to 127.0.0.1 only). False with a sentence in `error` for a bad port or a socket that would not open.
    bool start(int port, std::string& error, int packetFrames = net::kUdpPacketFramesShort) {
        if (running()) { stop(); }
        if (port < kMinPort || port > kMaxPort) {
            error = "The stream port must be between 1024 and 65535.";
            return false;
        }
        if (packetFrames != net::kUdpPacketFramesShort && packetFrames != net::kUdpPacketFramesLong) { packetFrames = net::kUdpPacketFramesShort; }
        std::vector<float> junk(256);
        while (ring_.read(junk.data(), junk.size()) > 0) {}  // an old stop leaves nothing behind
        dropped_.store(0);
        pushed_.store(0);
        return sender_.start("127.0.0.1", port, packetFrames,
                             [this](std::uint64_t&, float* out, std::size_t cap) { return static_cast<int>(ring_.read(out, cap)); }, error);
    }
    void stop() { sender_.stop(); }
    bool running() const { return sender_.running(); }

    // The gated audio of the channel. Nothing is queued while the gate is closed (silence is not sent) or while the sink is not started.
    void push(const float* audio, std::size_t n, bool gateOpen) {
        if (!running() || !gateOpen || audio == nullptr || n == 0) { return; }
        const std::size_t w = ring_.write(audio, n);
        pushed_.fetch_add(w);
        if (w < n) { dropped_.fetch_add(n - w); }
    }

    std::uint64_t packetsSent() const { return sender_.packetsSent(); }
    std::uint64_t bytesSent() const { return sender_.bytesSent(); }
    std::uint64_t sendErrors() const { return sender_.sendErrors(); }
    std::uint64_t droppedFrames() const { return dropped_.load(); }
    std::uint64_t queuedFrames() const { return pushed_.load(); }

private:
    dsp::SpscRing<float> ring_;
    net::UdpAudioSender sender_;
    std::atomic<std::uint64_t> dropped_{0}, pushed_{0};
};

}  // namespace cascade::core::airband
