// Playback transport for recorded I/Q: speed 0.25x to 8x, pause, single-step by N blocks, position, seek and an A-B loop state.
//
// STANDALONE. Nothing here is wired and no thread is started (docs/feature-records/recplay.md). It sits between a frame source (a recorded I/Q file
// reader) and the pipeline's pacing thread. The existing IqFileSource (source/iq_file_source.*) loops a whole file for ever with no seek, no pause and a
// fixed speed; feat/recording-models' PlaybackModel adds seek and loop and an audio-only speed, but refuses any speed for I/Q and has no pause or step.
//
// WHAT "SPEED" MEANS FOR I/Q (the pitch policy). The pipeline consumes frames at the file's sample rate (real time). Playing at speed s means the source
// advances s source frames for every frame delivered. Two ways, chosen by TransportConfig::policy:
//   * Varispeed (default): each delivered frame is the linear interpolation of the source at position p0 + k*s. Time runs s times faster, so every
//     signal frequency in the recording is multiplied by s: a carrier at +10 kHz plays at +20 kHz at 2x, an audio tone from an AM or FM demodulator rises
//     by s (like a tape). Nothing is dropped, the picture is continuous. Frequencies above (sample rate / 2) / s alias at s > 1: a linear interpolator
//     has no anti-alias filter, so at 8x only the lowest eighth of the band is clean; the owner may prefer BlockSkip for fast-forward.
//   * BlockSkip: the source is read in grains of grainFrames consecutive frames, and between grains the position jumps by grainFrames*(s-1) (s > 1,
//     content is skipped) or goes back to repeat part of the previous grain (s < 1, content is played again). Frequencies are NOT changed (a carrier at
//     +10 kHz stays at +10 kHz), but time is chopped: a demodulated voice is heard in short pieces with a click at each join (no cross-fade is built).
// At speed exactly 1 both policies deliver the source frames unchanged, in order.
//
// PAUSE AND STEP. pause() makes read() return 0 frames (paused = true) until resume(). While paused, stepBlocks(n) with n > 0 makes the next read() calls
// deliver exactly n*blockFrames frames (at the current speed and policy) and then return 0 again; n < 0 moves the position back |n|*blockFrames source
// frames (clamped to 0) and delivers one block from there. A step while playing is refused.
//
// THREADS AND THE "NO STALE BLOCK" RULE (same as PlaybackModel). ONE reader thread calls read(). Any thread may call the setters and position(). Seeks and
// step-backs increment the generation(); read() applies the request before producing anything and returns the generation it read under; a queue between the
// reader and the pipeline drops blocks whose generation differs from generation(). The audio-rate path (read()) allocates nothing: every buffer is sized in
// the constructor, and the only lock taken in read() is a short mutex hold when a control request is pending.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace cascade::source::transport {

using cf = std::complex<float>;

// The minimal source the transport needs. Adaptors for PlaybackModel's FrameSource are four lines (record, hand-off).
class TransportSource {
public:
    virtual ~TransportSource() = default;
    virtual std::uint64_t totalFrames() const = 0;
    virtual bool seek(std::uint64_t frame) = 0;                 // 0..total
    virtual std::size_t read(cf* dst, std::size_t n) = 0;       // fewer than n only at the end of the data
};

enum class SpeedPolicy : int { Varispeed = 0, BlockSkip = 1 };

constexpr double kMinSpeed = 0.25;
constexpr double kMaxSpeed = 8.0;
constexpr long long kMaxStepBlocks = 4096;

struct TransportConfig {
    SpeedPolicy policy = SpeedPolicy::Varispeed;
    std::size_t blockFrames = 4096;      // the unit of a step
    std::size_t grainFrames = 2048;      // BlockSkip: consecutive frames between jumps
    double sampleRateHz = 0.0;           // only for seconds in position(); 0 = unknown
};

struct ReadResult {
    std::size_t frames = 0;              // delivered (0 when paused with no step pending, or at the end)
    bool paused = false;
    bool atEnd = false;                  // the data ran out and no loop is set
    std::uint64_t generation = 0;        // the generation these frames were read under
};

struct Position {
    std::uint64_t frame = 0;             // the source frame about to be consumed
    std::uint64_t total = 0;
    double fraction = 0.0;
    double seconds = 0.0;                // 0 when the rate is unknown
    double speed = 1.0;
    bool paused = false;
    bool loopOn = false;
    std::uint64_t loopA = 0;
    std::uint64_t loopB = 0;
    std::uint64_t generation = 0;
    bool atEnd = false;
    long long stepFramesPending = 0;
};

class Transport {
public:
    Transport(TransportSource* source, const TransportConfig& cfg)
        : src_(source), cfg_(cfg) {
        if (cfg_.blockFrames == 0) { cfg_.blockFrames = 4096; }
        if (cfg_.grainFrames == 0) { cfg_.grainFrames = 2048; }
        total_ = src_ != nullptr ? src_->totalFrames() : 0;
        scratch_.assign(kScratch, cf(0.0f, 0.0f));   // the only allocation, here
        if (src_ != nullptr) { resetStream(0); }
    }

    // ---- control (any thread) --------------------------------------------------------------------------------------------------------------

    // False (and nothing changes) for NaN, infinity or a value outside 0.25 .. 8.
    bool setSpeed(double s) {
        if (!std::isfinite(s) || s < kMinSpeed || s > kMaxSpeed) { return false; }
        speed_.store(s, std::memory_order_release);
        return true;
    }
    double speed() const { return speed_.load(std::memory_order_acquire); }

    void pause() { paused_.store(true, std::memory_order_release); }
    void resume() {
        paused_.store(false, std::memory_order_release);
        stepFrames_.store(0, std::memory_order_release);   // a step left over from the pause must not fire later
    }
    bool paused() const { return paused_.load(std::memory_order_acquire); }

    // n > 0: deliver n blocks, then stay paused. n < 0: go back |n| blocks and deliver one. Refused (false) while playing, for n == 0 and for |n| over
    // kMaxStepBlocks.
    bool stepBlocks(long long n) {
        if (!paused() || n == 0 || n > kMaxStepBlocks || n < -kMaxStepBlocks) { return false; }
        if (n > 0) {
            stepFrames_.fetch_add(n * static_cast<long long>(cfg_.blockFrames), std::memory_order_acq_rel);
            return true;
        }
        const std::uint64_t back = static_cast<std::uint64_t>(-n) * cfg_.blockFrames;
        const std::uint64_t cur = posFrame_.load(std::memory_order_acquire);
        requestSeek(cur > back ? cur - back : 0);
        stepFrames_.fetch_add(static_cast<long long>(cfg_.blockFrames), std::memory_order_acq_rel);
        return true;
    }

    // Seek by frame (clamped to the total), seconds (needs the rate; NaN, infinity, negative refused) or fraction 0..1 (NaN refused). A refusal
    // leaves the position and the generation unchanged.
    bool seekFrame(std::uint64_t f) { requestSeek(std::min(f, total_)); return true; }
    bool seekSeconds(double s) {
        if (!std::isfinite(s) || s < 0.0 || cfg_.sampleRateHz <= 0.0) { return false; }
        const double f = s * cfg_.sampleRateHz;
        if (f >= static_cast<double>(total_)) { return seekFrame(total_); }
        return seekFrame(static_cast<std::uint64_t>(f));
    }
    bool seekFraction(double x) {
        if (!std::isfinite(x)) { return false; }
        x = std::min(1.0, std::max(0.0, x));
        const double f = x * static_cast<double>(total_);
        return seekFrame(f >= static_cast<double>(total_) ? total_ : static_cast<std::uint64_t>(f));
    }

    // Loop [a, b) with a < b <= total. False for anything else (the old loop stays).
    bool setLoop(std::uint64_t a, std::uint64_t b) {
        if (!(a < b) || b > total_) { return false; }
        std::lock_guard<std::mutex> lk(m_);
        loopOn_ = true; loopA_ = a; loopB_ = b; loopDirty_ = true;
        dirty_.store(true, std::memory_order_release);
        return true;
    }
    void clearLoop() {
        std::lock_guard<std::mutex> lk(m_);
        loopOn_ = false; loopDirty_ = true;
        dirty_.store(true, std::memory_order_release);
    }

    std::uint64_t generation() const { return gen_.load(std::memory_order_acquire); }
    std::uint64_t totalFrames() const { return total_; }

    Position position() const {
        Position p;
        p.total = total_;
        p.frame = std::min(posFrame_.load(std::memory_order_acquire), total_);
        p.fraction = total_ > 0 ? static_cast<double>(p.frame) / static_cast<double>(total_) : 0.0;
        p.seconds = cfg_.sampleRateHz > 0.0 ? static_cast<double>(p.frame) / cfg_.sampleRateHz : 0.0;
        p.speed = speed();
        p.paused = paused();
        {
            std::lock_guard<std::mutex> lk(m_);
            p.loopOn = loopOn_; p.loopA = loopA_; p.loopB = loopB_;
        }
        p.generation = generation();
        p.atEnd = atEnd_.load(std::memory_order_acquire);
        p.stepFramesPending = stepFrames_.load(std::memory_order_acquire);
        return p;
    }

    // ---- the reader thread -----------------------------------------------------------------------------------------------------------------

    ReadResult read(cf* dst, std::size_t n) {
        ReadResult r;
        if (src_ == nullptr || dst == nullptr) { r.generation = generation(); return r; }
        if (dirty_.load(std::memory_order_acquire)) { applyCommands(); }
        r.generation = gen_.load(std::memory_order_acquire);
        const bool isPaused = paused_.load(std::memory_order_acquire);
        r.paused = isPaused;
        if (isPaused) {
            const long long pend = stepFrames_.load(std::memory_order_acquire);
            if (pend <= 0 || n == 0) { r.atEnd = ended_; return r; }
            n = std::min<std::size_t>(n, static_cast<std::size_t>(pend));
        }
        if (n == 0) { r.atEnd = ended_; return r; }
        const std::size_t got = policy() == SpeedPolicy::Varispeed ? readVarispeed(dst, n) : readSkip(dst, n);
        r.frames = got;
        if (isPaused && got > 0) { stepFrames_.fetch_sub(static_cast<long long>(got), std::memory_order_acq_rel); }
        r.atEnd = ended_;
        atEnd_.store(ended_, std::memory_order_release);
        publishPosition();
        return r;
    }

private:
    static constexpr std::size_t kChunk = 2048;                                   // output frames per interpolation chunk
    static constexpr std::size_t kScratch = static_cast<std::size_t>(kChunk * kMaxSpeed) + 64;   // source frames one chunk can need

    SpeedPolicy policy() const { return cfg_.policy; }

    void requestSeek(std::uint64_t f) {
        std::lock_guard<std::mutex> lk(m_);
        seekPending_ = true; seekFrame_ = f;
        gen_.fetch_add(1, std::memory_order_acq_rel);
        posFrame_.store(f, std::memory_order_release);
        atEnd_.store(false, std::memory_order_release);
        dirty_.store(true, std::memory_order_release);
    }

    // Reader side: take the pending requests under the mutex, apply them outside it.
    void applyCommands() {
        bool doSeek = false, doLoop = false, on = false;
        std::uint64_t sf = 0, a = 0, b = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            dirty_.store(false, std::memory_order_release);
            doSeek = seekPending_; sf = seekFrame_; seekPending_ = false;
            doLoop = loopDirty_; on = loopOn_; a = loopA_; b = loopB_; loopDirty_ = false;
        }
        const std::uint64_t cur = currentFrame();
        if (doLoop) { loopActive_ = on; la_ = a; lb_ = b; }
        if (doSeek) {
            resetStream(sf);
        } else if (doLoop) {
            resetStream(cur);                  // frames read ahead before the loop was set must not outlive it; past B continues from A
        }
    }

    std::uint64_t currentFrame() const {
        if (cfg_.policy == SpeedPolicy::Varispeed) { return advance(srcPos_, static_cast<std::uint64_t>(rel_)); }
        return grainLeft_ == 0 ? static_cast<std::uint64_t>(skipPos_) : srcNext_;
    }

    void resetStream(std::uint64_t f) {
        f = std::min(f, total_);
        if (loopActive_ && f >= lb_) { f = la_; }
        srcPos_ = f; rel_ = 0.0; scratchLen_ = 0; eof_ = false; ended_ = false;
        skipPos_ = static_cast<double>(f); grainLeft_ = 0;
        reposition(f);
        posFrame_.store(f, std::memory_order_release);
        atEnd_.store(false, std::memory_order_release);
    }

    // Move the source cursor.
    void reposition(std::uint64_t f) {
        if (srcNextValid_ && srcNext_ == f) { return; }
        src_->seek(f);
        srcNext_ = f; srcNextValid_ = true;
    }

    // Source index reached after d frames from `from`, wrapping inside an active loop.
    std::uint64_t advance(std::uint64_t from, std::uint64_t d) const {
        if (loopActive_ && from >= la_ && from < lb_) { return la_ + (from - la_ + d) % (lb_ - la_); }
        return from + d;
    }

    // `want` sequential frames from the source cursor, wrapping at the loop end; fewer only at the end of the data (eof_ set).
    std::size_t readLooped(cf* dst, std::size_t want) {
        const std::uint64_t limit = loopActive_ ? lb_ : total_;
        std::size_t done = 0;
        while (done < want) {
            if (!srcNextValid_ || srcNext_ >= limit) {
                if (loopActive_) { reposition(la_); continue; }
                eof_ = true;
                break;
            }
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(want - done, limit - srcNext_));
            const std::size_t got = src_->read(dst + done, chunk);
            if (got == 0) { eof_ = true; break; }
            done += got; srcNext_ += got;
        }
        return done;
    }

    std::size_t readVarispeed(cf* dst, std::size_t n) {
        std::size_t produced = 0;
        while (produced < n && !ended_) {
            const std::size_t m = std::min(n - produced, kChunk);
            const double s = speed_.load(std::memory_order_acquire);
            const std::size_t need = static_cast<std::size_t>(std::floor(rel_ + static_cast<double>(m - 1) * s)) + 2;
            while (scratchLen_ < need && !eof_) {
                const std::size_t got = readLooped(scratch_.data() + scratchLen_, need - scratchLen_);
                scratchLen_ += got;
                if (got == 0) { break; }
            }
            std::size_t k = 0;
            for (; k < m; ++k) {
                const double pos = rel_ + static_cast<double>(k) * s;
                const std::size_t i = static_cast<std::size_t>(pos);
                if (i >= scratchLen_) { ended_ = true; break; }
                const double fr = pos - static_cast<double>(i);
                if (i + 1 >= scratchLen_) {
                    if (fr != 0.0) { ended_ = true; break; }      // between the last frame and nothing: the data is over
                    dst[produced + k] = scratch_[i];
                    continue;
                }
                const cf a = scratch_[i];
                const cf b = scratch_[i + 1];
                const float f = static_cast<float>(fr);
                dst[produced + k] = fr == 0.0 ? a : cf(a.real() + (b.real() - a.real()) * f, a.imag() + (b.imag() - a.imag()) * f);
            }
            produced += k;
            rel_ += static_cast<double>(k) * s;
            // drop the frames that can no longer be needed
            const std::size_t d = std::min(static_cast<std::size_t>(rel_), scratchLen_);
            if (d > 0) {
                std::memmove(scratch_.data(), scratch_.data() + d, (scratchLen_ - d) * sizeof(cf));
                scratchLen_ -= d;
                rel_ -= static_cast<double>(d);
                srcPos_ = advance(srcPos_, d);
            }
            if (ended_) { break; }
        }
        return produced;
    }

    std::size_t readSkip(cf* dst, std::size_t n) {
        std::size_t produced = 0;
        while (produced < n && !ended_) {
            if (grainLeft_ == 0) {
                double p = skipPos_;
                if (loopActive_) {
                    const double a = static_cast<double>(la_), b = static_cast<double>(lb_);
                    if (p >= b) { p = a + std::fmod(p - a, b - a); }
                } else if (p >= static_cast<double>(total_)) {
                    ended_ = true; break;
                }
                const std::uint64_t start = static_cast<std::uint64_t>(p);
                reposition(start);
                grainLeft_ = cfg_.grainFrames;
                skipPos_ = p + static_cast<double>(cfg_.grainFrames) * speed_.load(std::memory_order_acquire);
            }
            const std::size_t take = std::min(n - produced, grainLeft_);
            const std::size_t got = readLooped(dst + produced, take);
            produced += got; grainLeft_ -= got;
            if (got < take) { ended_ = true; break; }
        }
        return produced;
    }

    void publishPosition() { posFrame_.store(std::min(currentFrame(), total_), std::memory_order_release); }

    TransportSource* src_;
    TransportConfig cfg_;
    std::uint64_t total_ = 0;

    // control -> reader
    mutable std::mutex m_;
    std::atomic<bool> dirty_{false};
    bool seekPending_ = false;
    std::uint64_t seekFrame_ = 0;
    bool loopDirty_ = false;
    bool loopOn_ = false;
    std::uint64_t loopA_ = 0, loopB_ = 0;
    std::atomic<double> speed_{1.0};
    std::atomic<bool> paused_{false};
    std::atomic<long long> stepFrames_{0};
    std::atomic<std::uint64_t> gen_{1};

    // published by the reader
    std::atomic<std::uint64_t> posFrame_{0};
    std::atomic<bool> atEnd_{false};

    // reader-owned
    bool loopActive_ = false;
    std::uint64_t la_ = 0, lb_ = 0;
    std::vector<cf> scratch_;
    std::size_t scratchLen_ = 0;
    double rel_ = 0.0;                 // position inside scratch_ (Varispeed)
    std::uint64_t srcPos_ = 0;         // source index of scratch_[0]
    std::uint64_t srcNext_ = 0;        // the source's own cursor
    bool srcNextValid_ = false;
    bool eof_ = false;
    bool ended_ = false;
    double skipPos_ = 0.0;             // BlockSkip: start of the next grain
    std::size_t grainLeft_ = 0;
};

}  // namespace cascade::source::transport
