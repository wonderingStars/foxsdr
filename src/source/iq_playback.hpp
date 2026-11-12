// Playback position model over the I/Q readers (source/iq_readers.hpp) and a WAV audio source: seek by frame, by time and by fraction, an A-B loop
// that wraps without a gap, play speed for AUDIO files, position reporting for a seek bar, files over 4 GB (all positions are 64-bit), and a seek
// that never lets a stale block through.
//
// STANDALONE. Nothing here is wired and no thread is started (docs/feature-records/recording-models.md, Part 5). The existing IqFileSource
// (source/iq_file_source.*) loops a whole file for ever with no seek; this is what could replace its position logic.
//
// THREADS AND THE "NO STALE BLOCK" RULE
//   * ONE READER thread calls read() (the thread that paces the file into the pipeline). The file object is touched only there.
//   * ANY other thread (the screen, the engine's control thread) calls the seek / loop / speed setters and position(). They only record the request
//     under a short mutex (never during file I/O) and update the published position at once, so a seek bar jumps the moment it is clicked.
//   * Every seek increments the GENERATION. read() applies a pending seek before it produces anything, so the first block read after seek() returned
//     starts at the new position. A block that was already being read when the seek arrived carries the OLD generation in its ReadResult: a queue
//     between the reader and the pipeline drops every block whose generation differs from generation() (that is the complete rule; a block read before the
//     seek can never be told apart from a good one otherwise).
//   * The published position is only advanced by a read() whose generation is still current, so a slow read cannot move the bar back after a seek.
//
// LOOP. setLoop(a, b) with a < b <= total enables a loop; reading crosses b by seeking to a inside the same call, so the output has no missing or
// repeated frame at the wrap (frame b-1 is followed by frame a). A loop end moved below the play position, or a seek past b, continues from a.
// SPEED (audio files only; I/Q files refuse anything but 1.0): 0.25 .. 4.0 by linear interpolation of the source frames, i.e. like a tape: the pitch
// changes with the speed (a pitch-preserving stretch is not built). The reported position is the source frame about to be consumed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/rec_browser.hpp"
#include "source/iq_readers.hpp"

namespace cascade::source::playback {

using cf = std::complex<float>;

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual std::uint64_t totalFrames() const = 0;
    virtual double sampleRateHz() const = 0;      // 0 when unknown
    virtual bool isAudio() const = 0;
    virtual bool seek(std::uint64_t frame) = 0;   // 0..total; false past the end
    virtual std::size_t read(cf* dst, std::size_t n) = 0;
    virtual bool centerKnown() const { return false; }
    virtual double centerHz() const { return 0.0; }
};

class IqReaderSource final : public FrameSource {
public:
    explicit IqReaderSource(std::unique_ptr<iqfile::IqFileReader> r) : r_(std::move(r)) {}
    std::uint64_t totalFrames() const override { return r_->info().totalSamples; }
    double sampleRateHz() const override { return r_->info().sampleRateKnown ? r_->info().sampleRateHz : 0.0; }
    bool isAudio() const override { return false; }
    bool seek(std::uint64_t f) override { return r_->seek(f); }
    std::size_t read(cf* d, std::size_t n) override { return r_->read(d, n); }
    bool centerKnown() const override { return r_->info().centerKnown; }
    double centerHz() const override { return r_->info().centerFrequencyHz; }
    const iqfile::IqInfo& info() const { return r_->info(); }
private:
    std::unique_ptr<iqfile::IqFileReader> r_;
};

// A WAV (RIFF / RF64) audio file of 1..8 channels, PCM 8/16/24/32 or float32. Each frame is read as the MEAN of its channels in the real part.
class WavAudioSource final : public FrameSource {
public:
    static std::unique_ptr<WavAudioSource> open(const std::string& path, std::string* err) {
        std::error_code ec;
        const std::uint64_t size = std::filesystem::file_size(std::filesystem::path(path), ec);
        if (ec) { if (err != nullptr) { *err = "cannot read the file size"; } return nullptr; }
        const core::recbrowse::detail::AudioHead h = core::recbrowse::detail::probeWav(path, size);
        if (!h.ok) { if (err != nullptr) { *err = h.error; } return nullptr; }
        const bool pcm = h.tag == 1 && (h.bits == 8 || h.bits == 16 || h.bits == 24 || h.bits == 32);
        const bool flt = h.tag == 3 && h.bits == 32;
        if (!pcm && !flt) { if (err != nullptr) { *err = "unsupported sample format (" + std::to_string(h.bits) + " bit, tag " + std::to_string(h.tag) + ")"; } return nullptr; }
        if (h.blockAlign != static_cast<unsigned>(h.channels * (h.bits / 8))) { if (err != nullptr) { *err = "block align does not match channels and depth"; } return nullptr; }
        std::unique_ptr<WavAudioSource> s(new WavAudioSource());
#if defined(_WIN32)
        s->f_ = _wfopen(std::filesystem::path(path).c_str(), L"rb");
#else
        s->f_ = std::fopen(path.c_str(), "rb");
#endif
        if (s->f_ != nullptr) { std::setvbuf(s->f_, nullptr, _IONBF, 0); }   // unbuffered: reads are 8192 frames at a time, and a stale buffer would hide a file that shrank
        if (s->f_ == nullptr) { if (err != nullptr) { *err = "cannot open the file"; } return nullptr; }
        s->frames_ = h.frames; s->rate_ = h.rate; s->channels_ = h.channels; s->bits_ = h.bits; s->float_ = flt; s->dataOffset_ = h.dataOffset; s->align_ = h.blockAlign;
        return s;
    }
    std::uint64_t totalFrames() const override { return frames_; }
    double sampleRateHz() const override { return rate_; }
    bool isAudio() const override { return true; }
    bool seek(std::uint64_t f) override { if (f > frames_) { return false; } pos_ = f; return true; }
    std::size_t read(cf* d, std::size_t n) override {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(n, frames_ - pos_));
        std::size_t done = 0;
        buf_.resize(std::min<std::size_t>(want, 8192) * align_);
        while (done < want) {
            const std::size_t chunk = std::min<std::size_t>(want - done, 8192);
#if defined(_WIN32)
            _fseeki64(f_, static_cast<long long>(dataOffset_ + (pos_ + done) * align_), SEEK_SET);
#else
            fseeko(f_, static_cast<off_t>(dataOffset_ + (pos_ + done) * align_), SEEK_SET);
#endif
            const std::size_t gotBytes = std::fread(buf_.data(), 1, chunk * align_, f_);
            const std::size_t got = gotBytes / align_;
            for (std::size_t i = 0; i < got; ++i) {
                double acc = 0.0;
                const unsigned char* p = buf_.data() + i * align_;
                for (int c = 0; c < channels_; ++c) { acc += sample(p + static_cast<std::size_t>(c) * static_cast<std::size_t>(bits_ / 8)); }
                d[done + i] = cf(static_cast<float>(acc / channels_), 0.0f);
            }
            done += got;
            if (got < chunk) { frames_ = pos_ + done; break; }   // the file shrank: stop where the data stops
        }
        pos_ += done;
        return done;
    }
private:
    WavAudioSource() = default;
public:
    ~WavAudioSource() override { if (f_ != nullptr) { std::fclose(f_); } }
    WavAudioSource(const WavAudioSource&) = delete;
    WavAudioSource& operator=(const WavAudioSource&) = delete;
private:
    double sample(const unsigned char* p) const {
        if (float_) { float v; std::memcpy(&v, p, 4); return std::isfinite(v) ? static_cast<double>(v) : 0.0; }
        switch (bits_) {
            case 8: return (static_cast<int>(p[0]) - 128) / 128.0;
            case 16: return static_cast<std::int16_t>(p[0] | (p[1] << 8)) / 32768.0;
            case 24: { std::int32_t v = (p[0] << 8) | (p[1] << 16) | (static_cast<std::int32_t>(p[2]) << 24); return (v >> 8) / 8388608.0; }
            default: { std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24)); return v / 2147483648.0; }
        }
    }
    std::FILE* f_ = nullptr;
    std::uint64_t frames_ = 0, dataOffset_ = 0, pos_ = 0;
    double rate_ = 0.0;
    int channels_ = 1, bits_ = 16;
    bool float_ = false;
    std::size_t align_ = 2;
    std::vector<unsigned char> buf_;
};

struct PlaybackPosition {
    std::uint64_t frame = 0;
    std::uint64_t totalFrames = 0;
    double seconds = 0.0;          // 0 when the rate is unknown (rateKnown false)
    double durationS = 0.0;
    double fraction = 0.0;         // 0..1, for a seek bar
    bool rateKnown = false;
    bool loopEnabled = false;
    std::uint64_t loopA = 0, loopB = 0;
    double speed = 1.0;
    std::uint32_t generation = 0;
    bool atEnd = false;
};

struct ReadResult {
    std::size_t frames = 0;
    std::uint32_t generation = 0;      // drop the block when this differs from PlaybackModel::generation()
    std::uint64_t startFrame = 0;      // source frame of the first output frame
    bool endOfData = false;            // the data is exhausted and no loop is set
};

class PlaybackModel {
public:
    static constexpr double kMinSpeed = 0.25, kMaxSpeed = 4.0;

    bool open(std::unique_ptr<FrameSource> src, std::string* err) {
        if (src == nullptr) { if (err != nullptr) { *err = "no source"; } return false; }
        std::lock_guard<std::mutex> lk(mu_);
        src_ = std::move(src);
        total_ = src_->totalFrames();
        rate_ = src_->sampleRateHz();
        cmdPos_ = 0; seekPending_ = true; gen_ = 0; loopOn_ = false; loopA_ = loopB_ = 0; speed_ = 1.0; interpValid_ = false; phase_ = 0.0;
        pubFrame_.store(0); pubTotal_.store(total_);
        return true;
    }

    // ---- control (any thread) ----
    bool seekFrame(std::uint64_t frame) {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr) { return false; }
        cmdPos_ = std::min(frame, total_);
        seekPending_ = true;
        ++gen_;
        pubFrame_.store(cmdPos_);
        return true;
    }
    bool seekSeconds(double s) {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr || !(rate_ > 0.0) || !std::isfinite(s)) { return false; }
        const double fr = std::max(0.0, s) * rate_;
        cmdPos_ = fr >= static_cast<double>(total_) ? total_ : static_cast<std::uint64_t>(std::llround(fr));
        cmdPos_ = std::min(cmdPos_, total_);
        seekPending_ = true; ++gen_; pubFrame_.store(cmdPos_);
        return true;
    }
    bool seekFraction(double f) {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr || !std::isfinite(f)) { return false; }
        const double c = std::min(1.0, std::max(0.0, f));
        cmdPos_ = c >= 1.0 ? total_ : std::min(total_, static_cast<std::uint64_t>(std::llround(c * static_cast<double>(total_))));
        seekPending_ = true; ++gen_; pubFrame_.store(cmdPos_);
        return true;
    }
    // a < b <= total. Enables the loop. False (and nothing changes) otherwise.
    bool setLoop(std::uint64_t a, std::uint64_t b) {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr || a >= b || b > total_) { return false; }
        loopA_ = a; loopB_ = b; loopOn_ = true;
        return true;
    }
    bool setLoopSeconds(double a, double b) {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr || !(rate_ > 0.0) || !std::isfinite(a) || !std::isfinite(b) || a < 0.0) { return false; }
        const double fa = a * rate_, fb = b * rate_;
        if (fb > static_cast<double>(total_) + 0.5 || !(fa < fb)) { return false; }
        const std::uint64_t ia = static_cast<std::uint64_t>(std::llround(fa)), ib = std::min(total_, static_cast<std::uint64_t>(std::llround(fb)));
        if (ia >= ib) { return false; }
        loopA_ = ia; loopB_ = ib; loopOn_ = true;
        return true;
    }
    // The usual two-button use: mark A at the current position, then B. B at or before A is refused.
    bool markLoopStart() {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr) { return false; }
        markA_ = pubFrame_.load(); haveMarkA_ = true;
        return true;
    }
    bool markLoopEnd() {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr || !haveMarkA_) { return false; }
        const std::uint64_t b = pubFrame_.load();
        if (b <= markA_ || b > total_) { return false; }
        loopA_ = markA_; loopB_ = b; loopOn_ = true;
        return true;
    }
    void clearLoop() { std::lock_guard<std::mutex> lk(mu_); loopOn_ = false; haveMarkA_ = false; }
    // Audio sources only: 0.25 .. 4.0. I/Q sources accept 1.0 only.
    bool setSpeed(double v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_ == nullptr || !std::isfinite(v) || v < kMinSpeed || v > kMaxSpeed) { return false; }
        if (!src_->isAudio() && v != 1.0) { return false; }
        speed_ = v;
        return true;
    }

    // ---- reader thread ----
    ReadResult read(cf* dst, std::size_t n) {
        ReadResult rr;
        bool loopOn; std::uint64_t a, b; double speed; bool seek; std::uint64_t at; std::uint32_t gen;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (src_ == nullptr) { return rr; }
            gen = gen_; loopOn = loopOn_; a = loopA_; b = loopB_; speed = speed_; seek = seekPending_; at = cmdPos_;
            if (seek) { seekPending_ = false; }
        }
        rr.generation = gen;
        if (seek) {
            src_->seek(at);
            srcPos_ = at;
            interpValid_ = false; phase_ = 0.0;
        }
        if (loopOn && srcPos_ >= b) { src_->seek(a); srcPos_ = a; interpValid_ = false; phase_ = 0.0; }
        rr.startFrame = srcPos_;
        if (n == 0) { publish(gen, false); return rr; }
        std::size_t got = 0;
        if (speed == 1.0) {
            got = pull(dst, n, loopOn, a, b);
        } else {
            got = speedRead(dst, n, speed, loopOn, a, b);
        }
        rr.frames = got;
        const std::uint64_t total = src_->totalFrames();
        bool end = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (total != total_) { total_ = total; pubTotal_.store(total); if (loopOn_ && loopB_ > total_) { loopOn_ = false; } }   // the file shrank under us
            end = !loopOn_ && srcPos_ >= total_;
            if (gen_ == gen) { pubFrame_.store(srcPos_); }
        }
        rr.endOfData = got < n && end;
        pubAtEnd_.store(rr.endOfData);
        return rr;
    }

    // ---- any thread ----
    std::uint32_t generation() const { std::lock_guard<std::mutex> lk(mu_); return gen_; }
    PlaybackPosition position() const {
        std::lock_guard<std::mutex> lk(mu_);
        PlaybackPosition p;
        p.frame = pubFrame_.load();
        p.totalFrames = total_;
        p.rateKnown = rate_ > 0.0;
        p.seconds = p.rateKnown ? static_cast<double>(p.frame) / rate_ : 0.0;
        p.durationS = p.rateKnown ? static_cast<double>(total_) / rate_ : 0.0;
        p.fraction = total_ > 0 ? std::min(1.0, static_cast<double>(p.frame) / static_cast<double>(total_)) : 0.0;
        p.loopEnabled = loopOn_; p.loopA = loopA_; p.loopB = loopB_;
        p.speed = speed_; p.generation = gen_;
        p.atEnd = total_ > 0 && p.frame >= total_ && !loopOn_;
        return p;
    }

private:
    void publish(std::uint32_t gen, bool) {
        std::lock_guard<std::mutex> lk(mu_);
        if (gen_ == gen) { pubFrame_.store(srcPos_); }
    }

    // Reads n frames of the (possibly looping) source at speed 1; crossing the loop end wraps to the start inside the same call.
    std::size_t pull(cf* dst, std::size_t n, bool loopOn, std::uint64_t a, std::uint64_t b) {
        std::size_t got = 0;
        int guard = 0;
        while (got < n) {
            std::size_t want = n - got;
            if (loopOn) {
                if (srcPos_ >= b) { src_->seek(a); srcPos_ = a; }
                want = static_cast<std::size_t>(std::min<std::uint64_t>(want, b - srcPos_));
            }
            const std::size_t r = src_->read(dst + got, want);
            got += r; srcPos_ += r;
            if (r < want) { break; }                       // end of the data (or an unreadable file)
            if (loopOn && srcPos_ >= b) {
                src_->seek(a); srcPos_ = a;
                if (++guard > 1000000) { break; }
            }
        }
        return got;
    }

    // Linear interpolation at `speed` source frames per output frame, over the gapless pulled stream.
    std::size_t speedRead(cf* dst, std::size_t n, double speed, bool loopOn, std::uint64_t a, std::uint64_t b) {
        std::size_t got = 0;
        if (!interpValid_) {
            if (pull(&prev_, 1, loopOn, a, b) < 1) { return 0; }
            if (pull(&next_, 1, loopOn, a, b) < 1) { return 0; }
            interpValid_ = true; phase_ = 0.0;
        }
        while (got < n) {
            const float t = static_cast<float>(phase_);
            dst[got++] = prev_ + (next_ - prev_) * t;
            phase_ += speed;
            while (phase_ >= 1.0) {
                prev_ = next_;
                if (pull(&next_, 1, loopOn, a, b) < 1) { interpValid_ = false; return got; }
                phase_ -= 1.0;
            }
        }
        return got;
    }

    mutable std::mutex mu_;
    std::unique_ptr<FrameSource> src_;
    std::uint64_t total_ = 0;
    double rate_ = 0.0;
    // command state (under mu_)
    std::uint64_t cmdPos_ = 0;
    bool seekPending_ = false;
    std::uint32_t gen_ = 0;
    bool loopOn_ = false, haveMarkA_ = false;
    std::uint64_t loopA_ = 0, loopB_ = 0, markA_ = 0;
    double speed_ = 1.0;
    // reader-thread state
    std::uint64_t srcPos_ = 0;
    bool interpValid_ = false;
    double phase_ = 0.0;
    cf prev_{}, next_{};
    // published
    std::atomic<std::uint64_t> pubFrame_{0}, pubTotal_{0};
    std::atomic<bool> pubAtEnd_{false};
};

}  // namespace cascade::source::playback
