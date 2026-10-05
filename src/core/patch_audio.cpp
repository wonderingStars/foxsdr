// patch_audio.cpp - see patch_audio.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_audio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/mp3_writer.hpp"
#include "core/recorder.hpp"
#include "dsp/spsc_ring.hpp"
#include "sink/audio_out.hpp"
#include "sink/drift_matcher.hpp"

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

// THE FILE IS OPENED ON A WORKER, NEVER ON THE GUI THREAD (0.99.64). This used to
// be Recorder::start, called by AppWindow::patchPublishSets on the thread that
// draws the window: create the recordings folder, open the file, write and flush
// its header - three file-system calls that last as long as the disk takes, which
// for a synchronised or network folder, a drive that had spun down or a scanner
// holding the path is seconds (the freeze docs/DIAGNOSTICS.md describes for the
// Record button, 0.99.58). So start() prepares the request - no disk - and hands
// the blocking step, Recorder's opener, to a worker thread; it returns at once
// with a destination that already accepts sound.
//
// THE SOUND THAT ARRIVES WHILE THE FILE IS OPENING IS KEPT. write() runs on the
// radio's reader thread; until the worker has answered it appends to a bounded
// pre-roll (kPreRollSamples, 20 s), and the first write() after the answer arms
// the recorder on the opened file, ON THE READER THREAD - so Recorder keeps the
// one-writer-thread rule it was built for, begin() never overlaps a write - and
// writes the pre-roll ahead of the new block. A disk that takes longer than the
// pre-roll holds drops the overflow and says so (error()); one that does not
// answer at all costs the speaker its file and nothing else.
//
// THE WORKER OWNS WHAT IT TOUCHES BY VALUE (the request, the opener and a shared
// state it posts the answer into), so a destination destroyed while it is still
// inside the filesystem abandons it: the worker is detached, and the file it
// eventually opens is closed with the shared state, by whoever lets go last.
class WavDest final : public AudioDest {
public:
    // The pre-roll: sound offered before the file was open. 20 s of 48 kHz mono.
    static constexpr std::size_t kPreRollSamples = 20u * 48000u;
    // How long an open may be out, in seconds, before the face says so and the log
    // warns. Plain numbers and not std::chrono constants on purpose: a named chrono
    // duration in a file puts it under tests/test_shutdown_budget.cpp's scan for
    // bounded waits, and these are ages compared against a clock, not waits.
    static constexpr double kSlowAfterS = 1.0;
    static constexpr double kStuckAfterS = 5.0;

    // NEVER BLOCKS. False, with the reason, only when the request itself is
    // refused (nothing to do with the disk).
    bool start(const std::string& dir, const std::string& prefix, std::string& error,
               const Recorder::Opener& opener) {
        Recorder::OpenRequest req;
        if (!rec_.prepare(RecordKind::Audio, dir, kOutRateHz, prefix, req, error)) { return false; }
        path_ = req.path;
        shared_ = std::make_shared<Shared>();
        shared_->started = std::chrono::steady_clock::now();
        Recorder::Opener open = opener ? opener : rec_.opener();
        std::thread([sh = shared_, req = std::move(req), open = std::move(open)]() {
            Recorder::OpenedFile file;
            std::string err;
            bool ok = false;
            // A throw out of a worker would end the process.
            try {
                ok = open(req, file, err);
            } catch (...) {
                ok = false;
                err = "recorder: the file could not be opened";
            }
            if (!ok) { file = Recorder::OpenedFile{}; }
            const double tookS = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                               sh->started)
                                     .count();
            {
                std::lock_guard<std::mutex> lock(sh->m);
                sh->file = std::move(file);
                sh->ok = ok;
                sh->error = ok ? std::string() : err;
                sh->done = true;
            }
            sh->ready.store(true, std::memory_order_release);
            sh->cv.notify_all();
            // Neither line names the folder or the file, which are in the
            // user's profile.
            if (!ok) {
                diagWarnf("patch: a speaker's WAV file could not be opened");
            } else if (sh->stuckLogged.load()) {
                diagLogf("patch: a speaker's WAV file opened after %.0f s", tookS);
            }
        }).detach();
        return true;
    }

    // Waits for the open to answer and arms the recorder: for the callers that
    // CAN wait (makeWavDest, the tests), never the GUI thread.
    bool waitOpened(std::string& error) {
        {
            std::unique_lock<std::mutex> lock(shared_->m);
            shared_->cv.wait(lock, [this] { return shared_->done; });
        }
        arm();
        if (state_.load() == State::Failed) {
            std::lock_guard<std::mutex> lock(shared_->m);
            error = shared_->error;
            return false;
        }
        return true;
    }

    ~WavDest() override {
        if (state_.load() == State::Armed) {
            rec_.stop();  // patches the header: unchanged by 0.99.64
            return;
        }
        // Never armed: the sound still in the pre-roll is lost with the file,
        // which is a zero-sample WAV - what Record followed at once by Stop has
        // always left. The file, if the worker has already opened it, is
        // CLOSED ON A THREAD OF ITS OWN: a close is a file-system call too. One
        // the worker has not finished with is closed by the worker.
        Recorder::OpenedFile orphan;
        {
            std::lock_guard<std::mutex> lock(shared_->m);
            orphan = std::move(shared_->file);
        }
        if (orphan.file) {
            std::thread([f = std::move(orphan)]() mutable { f = Recorder::OpenedFile{}; })
                .detach();
        }
    }

    // THE READER THREAD, one thread only.
    void write(const float* s, std::size_t n) override {
        note(s, n);
        if (state_.load(std::memory_order_relaxed) == State::Opening &&
            shared_->ready.load(std::memory_order_acquire)) {
            arm();
        }
        switch (state_.load(std::memory_order_relaxed)) {
            case State::Armed:
                if (!preRoll_.empty()) {
                    rec_.writeAudio(preRoll_.data(), preRoll_.size());
                    std::vector<float>().swap(preRoll_);
                }
                rec_.writeAudio(s, n);
                break;
            case State::Opening: {
                const std::size_t room =
                    preRoll_.size() < kPreRollSamples ? kPreRollSamples - preRoll_.size() : 0;
                const std::size_t take = n < room ? n : room;
                preRoll_.insert(preRoll_.end(), s, s + take);
                if (take < n) { dropped_.fetch_add(n - take, std::memory_order_relaxed); }
                break;
            }
            case State::Failed:
                break;  // the file never opened; error() says why
        }
    }

    std::string describe() const override {
        std::string d = "WAV  " + baseName(path_);
        if (state_.load(std::memory_order_acquire) == State::Opening) {
            const double ageS = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                              shared_->started)
                                    .count();
            if (ageS >= kSlowAfterS) { d += " - waiting for the disk to open it"; }
            if (ageS >= kStuckAfterS && !shared_->stuckLogged.exchange(true)) {
                diagWarnf("patch: a speaker's WAV file has not opened for %.0f s - the open is "
                          "waiting on a worker thread, the window is not",
                          ageS);
            }
        }
        return d;
    }

    std::string error() const override {
        switch (state_.load(std::memory_order_acquire)) {
            case State::Failed: {
                std::lock_guard<std::mutex> lock(shared_->m);
                return shared_->error;
            }
            case State::Armed:
                if (rec_.sizeLimitReached()) {
                    return "the WAV file reached its 4 GB limit and was closed";
                }
                if (rec_.writeFailed()) { return "the disk refused a write - is it full?"; }
                break;
            case State::Opening:
                break;
        }
        if (dropped_.load(std::memory_order_relaxed) > 0) {
            return "the file took too long to open and some sound was not written";
        }
        return {};
    }

private:
    enum class State { Opening, Armed, Failed };

    // What the worker posts and the destination reads: the answer, and the file.
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        std::string error;
        Recorder::OpenedFile file;
        std::atomic<bool> ready{false};  // `done`, for the reader thread's lock-free check
        std::atomic<bool> stuckLogged{false};
        std::chrono::steady_clock::time_point started;
    };

    // The worker has answered: arm the recorder on its file, or record that it
    // could not be opened. Called by whichever thread is the writer.
    void arm() {
        Recorder::OpenedFile file;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(shared_->m);
            ok = shared_->ok;
            file = std::move(shared_->file);
        }
        std::string err;
        if (ok && rec_.begin(std::move(file), err)) {
            state_.store(State::Armed, std::memory_order_release);
            return;
        }
        if (ok) {
            std::lock_guard<std::mutex> lock(shared_->m);
            shared_->error = err;
        }
        state_.store(State::Failed, std::memory_order_release);
    }

    Recorder rec_;
    std::string path_;
    std::shared_ptr<Shared> shared_;
    std::atomic<State> state_{State::Opening};
    std::vector<float> preRoll_;  // the writer thread's alone
    std::atomic<std::uint64_t> dropped_{0};
};

// --- MP3 ---------------------------------------------------------------------

class Mp3Dest final : public AudioDest {
public:
    // NEVER TOUCHES THE DISK ON THE CALLING THREAD (0.99.64): the recordings
    // folder is made by the worker, just before it opens the file. It used to be
    // made here, by makeMp3Dest, on the GUI thread - create_directories and
    // is_directory, which a slow folder holds for as long as it likes. A folder
    // that cannot be made is now reported through error(), in the words it always
    // had; the WAV fallback is for the build that has no encoder, which needs no
    // disk to find out.
    Mp3Dest(std::string directory, std::string path,
            std::function<bool(const std::string&)> makeDirectory)
        : directory_(std::move(directory)),
          path_(std::move(path)),
          makeDirectory_(std::move(makeDirectory)),
          ring_(kRingFrames) {
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
        bool folderOk = false;
        if (makeDirectory_) {
            folderOk = makeDirectory_(directory_);
        } else {
            std::error_code ec;
            std::filesystem::create_directories(directory_, ec);
            folderOk = !ec && std::filesystem::is_directory(directory_);
        }
        if (!folderOk) {
            std::lock_guard<std::mutex> lock(m_);
            error_ = "cannot create the recordings folder \"" + directory_ + "\"";
            return;   // write() keeps filling the ring, which simply overflows
        }
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

    std::string directory_;
    std::string path_;
    std::function<bool(const std::string&)> makeDirectory_;
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
        // The patch's radio and this sound card keep different clocks, the
        // same as the receiver's own sink: hold the lead with the same
        // matcher (sink/drift_matcher.hpp) rather than let each hiccup eat
        // into it for good.
        matcher_.observe(out_.ringFrames(), out_.running() && out_.primed(), n);
        const std::size_t cap = sink::DriftMatcher::maxOut(n);
        matched_.resize(cap);
        out_.write(matched_.data(), matcher_.process(s, n, 1, matched_.data(), cap));
    }
    std::string describe() const override { return "Playing on " + label_; }
    std::string error() const override {
        return out_.streamAlive() ? std::string{} : "the sound output stopped";
    }

private:
    sink::AudioOut out_;
    sink::DriftMatcher matcher_{kOutRateHz};
    std::vector<float> matched_;
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

std::shared_ptr<AudioDest> makeWavDestAsync(const std::string& directory,
                                            const std::string& prefix, std::string& error,
                                            const DestSeams* seams) {
    auto d = std::make_shared<WavDest>();
    if (!d->start(directory, prefix, error,
                  seams != nullptr ? seams->wavOpener : Recorder::Opener{})) {
        return nullptr;
    }
    return d;
}

std::shared_ptr<AudioDest> makeWavDest(const std::string& directory, const std::string& prefix,
                                       std::string& error, const DestSeams* seams) {
    auto d = std::make_shared<WavDest>();
    if (!d->start(directory, prefix, error,
                  seams != nullptr ? seams->wavOpener : Recorder::Opener{})) {
        return nullptr;
    }
    // THE ONE PLACE THAT WAITS, for the callers that can: the file is open, or the
    // reason it is not, before this returns.
    if (!d->waitOpened(error)) { return nullptr; }
    return d;
}

std::shared_ptr<AudioDest> makeMp3Dest(const std::string& directory, const std::string& prefix,
                                       std::string& error, const DestSeams* seams) {
    if (!Mp3Writer::available()) {
        error = "MP3 needs Windows' own encoder; this build writes WAV instead";
        return nullptr;
    }
    // No disk here: the folder is the worker's (see Mp3Dest).
    return std::make_shared<Mp3Dest>(directory, timestampedPath(directory, prefix, "mp3"),
                                     seams != nullptr ? seams->makeDirectory
                                                      : std::function<bool(const std::string&)>{});
}

std::shared_ptr<AudioDest> makeDeviceDest(const std::string& deviceName, std::string& error) {
    auto d = std::make_shared<DeviceDest>();
    if (!d->open(deviceName, error)) { return nullptr; }
    return d;
}

}  // namespace cascade::core::patch
