// patch_audio.cpp - see patch_audio.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "core/mp3_writer.hpp"
#include "core/recorder.hpp"
#include "dsp/spsc_ring.hpp"
#include "sink/audio_out.hpp"

namespace cascade::core::patch {

std::string fileSafeName(const std::string& name) {
    std::string out;
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(keep ? static_cast<char>(c) : '_');
        if (out.size() >= 40) { break; }
    }
    if (out.empty()) { out = "speaker"; }
    return out;
}

std::string patchFilePrefix(unsigned node, const std::string& name) {
    return "patch-" + std::to_string(node) + "-" + fileSafeName(name);
}

namespace {

std::string baseName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

// --- WAV ---------------------------------------------------------------------

class WavDest final : public AudioDest {
public:
    bool start(const std::string& dir, const std::string& prefix, std::string& error) {
        return rec_.start(RecordKind::Audio, dir, kOutRateHz, error, prefix);
    }
    ~WavDest() override { rec_.stop(); }
    void write(const float* s, std::size_t n) override {
        note(s, n);
        rec_.writeAudio(s, n);
    }
    std::string describe() const override { return "WAV  " + baseName(rec_.path()); }
    std::string error() const override {
        if (rec_.sizeLimitReached()) { return "the WAV file reached its 4 GB limit and was closed"; }
        if (rec_.writeFailed()) { return "the disk refused a write - is it full?"; }
        return {};
    }

private:
    Recorder rec_;
};

// --- MP3 ---------------------------------------------------------------------

class Mp3Dest final : public AudioDest {
public:
    explicit Mp3Dest(std::string path) : path_(std::move(path)), ring_(kRingFrames) {
        worker_ = std::thread([this] { run(); });
    }
    ~Mp3Dest() override {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) { worker_.join(); }
    }
    void write(const float* s, std::size_t n) override {
        note(s, n);
        // Never blocks: if the writer thread has fallen five seconds behind,
        // the overflow is dropped and counted rather than stalling the radio.
        const std::size_t took = ring_.write(s, n);
        if (took < n) { dropped_.fetch_add(n - took, std::memory_order_relaxed); }
    }
    std::string describe() const override { return "MP3  " + baseName(path_); }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(m_);
        if (!error_.empty()) { return error_; }
        if (dropped_.load(std::memory_order_relaxed) > 0) {
            return "the MP3 encoder fell behind and some sound was not written";
        }
        return {};
    }

private:
    static constexpr std::size_t kRingFrames = std::size_t{1} << 18;   // ~5.5 s at 48 kHz

    void run() {
        Mp3Writer w;
        std::string err;
        if (!w.open(path_, static_cast<unsigned>(kOutRateHz), 1, 128, err)) {
            std::lock_guard<std::mutex> lock(m_);
            error_ = err;
            return;   // write() keeps filling the ring, which simply overflows
        }
        std::vector<float> f(8192);
        std::vector<std::int16_t> pcm(8192);
        const auto drain = [&] {
            for (;;) {
                const std::size_t n = ring_.read(f.data(), f.size());
                if (n == 0) { return; }
                for (std::size_t i = 0; i < n; ++i) {
                    const float v = std::clamp(f[i], -1.0f, 1.0f);
                    pcm[i] = static_cast<std::int16_t>(std::lround(v * 32767.0f));
                }
                if (!w.write(pcm.data(), n)) {
                    std::lock_guard<std::mutex> lock(m_);
                    if (error_.empty()) { error_ = "the MP3 encoder refused a write"; }
                }
            }
        };
        for (;;) {
            bool stopping = false;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stop_; });
                stopping = stop_;
            }
            drain();
            if (stopping) { break; }
        }
        w.close();
    }

    std::string path_;
    dsp::SpscRing<float> ring_;
    std::thread worker_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::string error_;
    std::atomic<std::uint64_t> dropped_{0};
};

// --- a sound device ------------------------------------------------------------

class DeviceDest final : public AudioDest {
public:
    bool open(const std::string& name, std::string& error) {
        const std::vector<sink::AudioDevice> devs = out_.listOutputDevices();
        int index = -1;
        for (const sink::AudioDevice& d : devs) {
            if (name.empty() ? d.isDefault : d.name == name) {
                index = d.index;
                label_ = d.name;
                break;
            }
        }
        if (index < 0) {
            error = name.empty() ? "no default sound output was found"
                                 : "the sound output \"" + name + "\" is not connected";
            return false;
        }
        if (!out_.open(index, kOutRateHz, 1)) {
            error = "the sound output \"" + label_ + "\" would not open";
            return false;
        }
        return true;
    }
    void write(const float* s, std::size_t n) override {
        note(s, n);
        out_.write(s, n);
    }
    std::string describe() const override { return "Playing on " + label_; }
    std::string error() const override {
        return out_.streamAlive() ? std::string{} : "the sound output stopped";
    }

private:
    sink::AudioOut out_;
    std::string label_;
};

std::string timestampedPath(const std::string& dir, const std::string& prefix,
                            const char* ext) {
    std::tm tmv{};
    const std::time_t now = std::time(nullptr);
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[48];
    std::snprintf(stamp, sizeof stamp, "_%04d%02d%02d_%02d%02d%02d.%s", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ext);
    return (std::filesystem::path(dir) / (prefix + stamp)).string();
}

}  // namespace

std::shared_ptr<AudioDest> makeWavDest(const std::string& directory, const std::string& prefix,
                                       std::string& error) {
    auto d = std::make_shared<WavDest>();
    if (!d->start(directory, prefix, error)) { return nullptr; }
    return d;
}

std::shared_ptr<AudioDest> makeMp3Dest(const std::string& directory, const std::string& prefix,
                                       std::string& error) {
    if (!Mp3Writer::available()) {
        error = "MP3 needs Windows' own encoder; this build writes WAV instead";
        return nullptr;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec || !std::filesystem::is_directory(directory)) {
        error = "cannot create the recordings folder \"" + directory + "\"";
        return nullptr;
    }
    return std::make_shared<Mp3Dest>(timestampedPath(directory, prefix, "mp3"));
}

std::shared_ptr<AudioDest> makeDeviceDest(const std::string& deviceName, std::string& error) {
    auto d = std::make_shared<DeviceDest>();
    if (!d->open(deviceName, error)) { return nullptr; }
    return d;
}

}  // namespace cascade::core::patch
