// airband_gate.hpp - THE AIRBAND MONITOR'S PER-CHANNEL SETTINGS, wired (wire-a second pass, item 6). core/airband_channel.hpp is the model (the config, its text form FOXAIRCH1, the filter chain and the squelch);
// core/airband_writer.hpp the two outputs. This is the glue the monitor's set builder needs: a patch::ChannelGate for one channel, and the table that says which channels have settings of their own.
//
// WHAT A CHANNEL WITH ITS OWN SETTINGS DOES, differently from the monitor's shared squelch: its high-pass, low-pass and notches run on the demodulated audio first; its own squelch (a LEVEL threshold with its own hold,
// or an SNR threshold with hysteresis) gates the audio; and, only if the user switched one on for THAT channel, the audio before the gate goes to a WAV file (written on a worker thread, in the recordings folder, a
// folder per UTC day) or the gated audio to a UDP stream on 127.0.0.1 and the port chosen. Both outputs are OFF unless asked for; a channel with no row in the table is the monitor's channel exactly as before.
//
// THREADS. process() runs on the audio thread: no allocation, no lock it can wait on (the file output hands its blocks to its worker through a try-lock and DROPS the block when the lock is busy, counted), no file or
// socket call. The worker and the UDP sender are threads of their own, started when the set is built and joined when it is destroyed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

// The WAV output of one channel: blocks of pre-gate audio and the gate's state, handed over by try-lock, written by a worker thread.
class AsyncWavOutput {
public:
    explicit AsyncWavOutput(const WriterConfig& cfg) : writer_(cfg) { thread_ = std::thread([this] { run(); }); }
    ~AsyncWavOutput() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) { thread_.join(); }
        writer_.close();
    }
    AsyncWavOutput(const AsyncWavOutput&) = delete;
    AsyncWavOutput& operator=(const AsyncWavOutput&) = delete;

    // Audio thread. Drops (and counts) the block when the worker holds the lock or the queue is long.
    void push(const float* audio, std::size_t n, bool open) {
        std::unique_lock<std::mutex> lk(m_, std::try_to_lock);
        if (!lk.owns_lock() || queue_.size() >= kMaxQueued) {
            dropped_.fetch_add(n, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(Block{std::vector<float>(audio, audio + n), open});   // (one allocation per block: the audio thread's price for an output that was asked for)
        lk.unlock();
        cv_.notify_one();
    }
    std::uint64_t droppedFrames() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t writtenFrames() const { return written_.load(std::memory_order_relaxed); }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

private:
    struct Block {
        std::vector<float> audio;
        bool open = false;
    };
    static constexpr std::size_t kMaxQueued = 256;
    void run() {
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) { return; }   // stopping, and nothing left
                b = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!writer_.write(b.audio.data(), b.audio.size(), b.open, utcNow())) { failed_.store(true, std::memory_order_relaxed); }
            written_.fetch_add(b.audio.size(), std::memory_order_relaxed);
        }
    }
    ChannelWavWriter writer_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Block> queue_;
    bool stop_ = false;
    std::atomic<std::uint64_t> dropped_{0}, written_{0};
    std::atomic<bool> failed_{false};
    std::thread thread_;
};

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
