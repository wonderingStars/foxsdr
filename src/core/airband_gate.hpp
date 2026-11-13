// airband_gate.hpp - THE AIRBAND MONITOR'S PER-CHANNEL SETTINGS, wired (wire-a second pass, item 6). core/airband_channel.hpp is the model (the config, its text form FOXAIRCH1, the filter chain and the squelch);
// core/airband_writer.hpp the two outputs. This is the glue the monitor's set builder needs: a patch::ChannelGate for one channel, and the table that says which channels have settings of their own.
//
// WHAT A CHANNEL WITH ITS OWN SETTINGS DOES, differently from the monitor's shared squelch: its high-pass, low-pass and notches run on the demodulated audio first; its own squelch (a LEVEL threshold with its own hold,
// or an SNR threshold with hysteresis) gates the audio; and, only if the user switched one on for THAT channel, the audio before the gate goes to a WAV file (written on a worker thread, in the recordings folder, a
// folder per UTC day) or the gated audio to a UDP stream on 127.0.0.1 and the port chosen. Both outputs are OFF unless asked for; a channel with no row in the table is the monitor's channel exactly as before.
//
// THREADS. process() runs on the audio thread: no allocation, no lock it can wait on (the file output copies its blocks into a ring allocated when the set was built, through a try-lock, and DROPS the block when the lock is busy or the ring is full, counted), no file or
// socket call. The UDP sender is a thread of its own, started when the set is built and joined when it is destroyed. The file output's worker is NOT joined: destroying the output (on the control thread, when a plan is
// retired) only tells it to finish, and it writes what is queued, closes the file and ends by itself (see BasicAsyncWavOutput).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/airband_channel.hpp"
#include "core/airband_writer.hpp"
#include "core/patch_runner.hpp"
#include "core/record_finish.hpp"

namespace cascade::core::airband {

inline UtcStamp utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    UtcStamp s;
    s.year = tm.tm_year + 1900;
    s.month = tm.tm_mon + 1;
    s.day = tm.tm_mday;
    s.hour = tm.tm_hour;
    s.minute = tm.tm_min;
    s.second = tm.tm_sec;
    return s;
}

// The WAV output of one channel: blocks of pre-gate audio and the gate's state, handed over by try-lock into a pre-allocated ring, written by a worker thread.
// A template on the writer only so that a test can put a writer that blocks on demand in its place (the product uses ChannelWavWriter).
//
// DESTROYING IT NEVER WAITS FOR THE DISK (engine fixes 6). The gate that owns it lives in a patch plan, and a retired plan is destroyed on the control
// thread: a destructor that joined the worker and closed the file waited for the disk on the thread that applies the person's commands. So the worker
// is detached and owns everything it touches (the shared state below: ring, writer, flags) by shared pointer; the destructor only tells it to stop and
// returns. The worker writes what is still queued, closes the file, and ends - the same shape as the patch speaker's MP3 destination
// (core/patch_audio.cpp). The retirement is counted with the recorders' finishes (RecordFinisher::externalBegin / externalEnd), so that a quit gives
// the close the same bounded chance (drain(deadline)) and never waits past it; a close that does not land leaves the header the burst's last block
// patched, which is what a crash would have left.
template <class Writer>
class BasicAsyncWavOutput {
public:
    // The hand-over: up to kMaxQueued blocks in a ring of kRingFloats samples, allocated here (the control thread), never on the audio thread.
    static constexpr std::size_t kMaxQueued = 256;
    static constexpr std::size_t kRingFloats = std::size_t{1} << 18;   // 5.4 s at 48 kHz

    explicit BasicAsyncWavOutput(const WriterConfig& cfg) : s_(std::make_shared<Shared>(cfg)) {
        std::thread([s = s_] { work(*s); }).detach();
    }
    ~BasicAsyncWavOutput() {
        {
            std::lock_guard<std::mutex> lk(s_->m);
            s_->stop = true;
            if (!s_->ended) {
                s_->counted = true;
                RecordFinisher::externalBegin();
            }
        }
        s_->cv.notify_all();
    }
    BasicAsyncWavOutput(const BasicAsyncWavOutput&) = delete;
    BasicAsyncWavOutput& operator=(const BasicAsyncWavOutput&) = delete;

    // Audio thread. No allocation, no wait: drops (and counts) the block when the worker holds the lock, the queue is full, or the ring cannot hold it.
    void push(const float* audio, std::size_t n, bool open) {
        if (n == 0) { return; }
        Shared& s = *s_;
        std::unique_lock<std::mutex> lk(s.m, std::try_to_lock);
        if (!lk.owns_lock() || s.count >= kMaxQueued || n > kRingFloats - s.used) {
            s.dropped.fetch_add(n, std::memory_order_relaxed);
            return;
        }
        const std::size_t first = std::min(n, kRingFloats - s.tail);
        std::memcpy(s.ring.data() + s.tail, audio, first * sizeof(float));
        if (n > first) { std::memcpy(s.ring.data(), audio + first, (n - first) * sizeof(float)); }
        s.meta[(s.mhead + s.count) % kMaxQueued] = Meta{s.tail, n, open};
        s.tail = (s.tail + n) % kRingFloats;
        s.used += n;
        ++s.count;
        lk.unlock();
        s.cv.notify_one();
    }
    std::uint64_t droppedFrames() const { return s_->dropped.load(std::memory_order_relaxed); }
    std::uint64_t writtenFrames() const { return s_->written.load(std::memory_order_relaxed); }
    bool failed() const { return s_->failed.load(std::memory_order_relaxed); }

private:
    struct Meta {
        std::size_t at = 0;
        std::size_t len = 0;
        bool open = false;
    };
    struct Shared {
        explicit Shared(const WriterConfig& cfg) : writer(cfg), ring(kRingFloats, 0.0f), meta(kMaxQueued) {}
        Writer writer;
        std::mutex m;
        std::condition_variable cv;
        std::vector<float> ring;
        std::vector<Meta> meta;
        std::size_t tail = 0;     // where the next block goes
        std::size_t used = 0;     // samples held (a block being written is held until it is done)
        std::size_t mhead = 0;    // the oldest block's slot in `meta`
        std::size_t count = 0;    // blocks held
        bool stop = false;        // the output was destroyed: write what is left, close, end
        bool ended = false;       // the worker has finished (its file closed)
        bool counted = false;     // the retirement is counted in RecordFinisher until `ended`
        std::atomic<std::uint64_t> dropped{0}, written{0};
        std::atomic<bool> failed{false};
    };
    // The worker's last act, however it ends: tell the quit drain it is done.
    struct EndGuard {
        Shared& s;
        ~EndGuard() {
            std::lock_guard<std::mutex> lk(s.m);
            s.ended = true;
            if (s.counted) {
                s.counted = false;
                RecordFinisher::externalEnd();
            }
        }
    };
    static void work(Shared& s) {
        const EndGuard guard{s};
        std::vector<float> block;   // the worker's own copy of a block (it may allocate: it is not the audio thread)
        for (;;) {
            Meta mt;
            {
                std::unique_lock<std::mutex> lk(s.m);
                s.cv.wait(lk, [&s] { return s.stop || s.count > 0; });
                if (s.count == 0) { break; }   // stopping, and nothing left
                mt = s.meta[s.mhead];
            }
            // The block stays held (the producer cannot overwrite it) while it is copied out and written, so no lock is held across the disk.
            block.resize(mt.len);
            const std::size_t first = std::min(mt.len, kRingFloats - mt.at);
            std::memcpy(block.data(), s.ring.data() + mt.at, first * sizeof(float));
            if (mt.len > first) { std::memcpy(block.data() + first, s.ring.data(), (mt.len - first) * sizeof(float)); }
            if (!s.writer.write(block.data(), block.size(), mt.open, utcNow())) { s.failed.store(true, std::memory_order_relaxed); }
            s.written.fetch_add(mt.len, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(s.m);
                s.mhead = (s.mhead + 1) % kMaxQueued;
                --s.count;
                s.used -= mt.len;
            }
        }
        s.writer.close();
    }
    std::shared_ptr<Shared> s_;
};
using AsyncWavOutput = BasicAsyncWavOutput<ChannelWavWriter>;

// One channel's gate: filters, then its own squelch, then its outputs.
class ChannelGateImpl final : public patch::ChannelGate {
public:
    ChannelGateImpl(const AirbandChannelConfig& cfg, double audioRateHz, const std::string& recordingsRoot, int channelIndex)
        : cfg_(cfg), proc_(audioRateHz), preGate_(patch::kMaxBlockAudio, 0.0f) {
        proc_.configure(cfg_);
        if (cfg_.output == OutputKind::WavFile && !recordingsRoot.empty()) {
            WriterConfig w;
            w.recordingsRoot = recordingsRoot;
            w.channelIndex = channelIndex;
            w.freqHz = static_cast<double>(cfg_.freqHz);
            w.dailyFolder = cfg_.dailyFolder;
            w.maxFileBytes = static_cast<std::uint64_t>(cfg_.maxFileMb) * 1024ull * 1024ull;
            w.sampleRate = static_cast<int>(audioRateHz + 0.5);
            wav_ = std::make_unique<AsyncWavOutput>(w);
        } else if (cfg_.output == OutputKind::Udp && cfg_.udpPort != 0) {
            udp_ = std::make_unique<LoopbackAudioSink>();
            std::string err;
            if (!udp_->start(cfg_.udpPort, err)) { udp_.reset(); streamError_ = err; }
        }
    }
    bool process(const std::complex<float>* baseband, float* audio, std::size_t n) override {
        if (n > preGate_.size()) { n = preGate_.size(); }
        const bool open = proc_.process(baseband, audio, n, wav_ ? preGate_.data() : nullptr);
        open_ = open;
        level_ = static_cast<float>(proc_.squelch().meterDb());
        if (wav_) { wav_->push(preGate_.data(), n, open); }
        if (udp_) { udp_->push(audio, n, open); }
        return open;
    }
    float levelDb() const override { return level_; }
    bool isOpen() const override { return open_; }
    const AirbandChannelConfig& config() const { return cfg_; }
    bool fileOutput() const { return wav_ != nullptr; }
    bool streamOutput() const { return udp_ != nullptr && udp_->running(); }
    const std::string& streamError() const { return streamError_; }
    const AsyncWavOutput* wavOut() const { return wav_.get(); }
    const LoopbackAudioSink* sink() const { return udp_.get(); }

private:
    AirbandChannelConfig cfg_;
    ChannelProcessor proc_;
    std::vector<float> preGate_;
    std::unique_ptr<AsyncWavOutput> wav_;
    std::unique_ptr<LoopbackAudioSink> udp_;
    std::string streamError_;
    float level_ = -200.0f;
    bool open_ = false;
};

// Which channels have settings of their own: keyed by the frequency in whole hertz (a monitor row is a frequency). The table is read when a set is built.
class ChannelTable {
public:
    // False (nothing changes) for a config validate() refuses.
    bool set(const AirbandChannelConfig& c) {
        if (validate(c) != Problem::None) { return false; }
        map_[c.freqHz] = c;
        return true;
    }
    bool clear(std::int64_t freqHz) { return map_.erase(freqHz) != 0; }
    void clearAll() { map_.clear(); }
    const AirbandChannelConfig* find(double freqHz) const {
        const auto it = map_.find(static_cast<std::int64_t>(freqHz + (freqHz >= 0 ? 0.5 : -0.5)));
        return it == map_.end() ? nullptr : &it->second;
    }
    std::size_t size() const { return map_.size(); }
    const std::map<std::int64_t, AirbandChannelConfig>& all() const { return map_; }
    bool operator==(const ChannelTable& o) const { return map_ == o.map_; }

    // The factory the set builder takes. `root` is the recordings folder the file output writes under. The table is COPIED into the closure: a set already built does not change under a later edit.
    MonitorGateFactory factory(const std::string& root) const {
        if (map_.empty()) { return MonitorGateFactory(); }
        const ChannelTable snap = *this;
        return [snap, root](const MonitorChannel& c, std::size_t index, double outRateHz) -> std::unique_ptr<patch::ChannelGate> {
            const AirbandChannelConfig* cfg = snap.find(c.freqHz);
            if (cfg == nullptr) { return nullptr; }
            return std::make_unique<ChannelGateImpl>(*cfg, outRateHz, root, static_cast<int>(index) + 1);
        };
    }

private:
    std::map<std::int64_t, AirbandChannelConfig> map_;
};

}  // namespace cascade::core::airband
