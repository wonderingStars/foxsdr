// Record only while the squelch is open: the logic that turns squelch open/close events plus a
// pre-roll from the replay ring into time-stamped clip files, with hang time and a minimum length.
//
// STANDALONE BLOCK. Nothing here is wired into the pipeline or the engine
// (docs/feature-records/signal-blocks.md, "Wiring hand-off", says where it goes). Design:
// pre-release-features-design.md section 3. It does NOT use core::Recorder's pipeline tap (that is the
// manual take, which also runs): it reads finished audio out of a core::ReplayRing and writes it with
// core::Recorder's own WAV file machinery (the same header, the same exclusive-create naming, the
// same crash contract: a clip cut short by a crash is a valid zero-length-header husk).
//
// TWO CLASSES, TWO THREAD RULES.
//
//   ClipTrigger (PASS thread, i.e. the engine's per-radio poll; never the DSP thread and never a
//   worker): update(gateOpen, headSample, unixNow, frequencyHz, mode, out, cap) is called once a pass
//   with the squelch gate (the published carrier/tone gate) and ReplayRing::head(), and appends at
//   most two ClipCmd to `out`. It does arithmetic on a handful of integers: no allocation, no lock, no
//   file, no wait. endNow() closes an open clip (a stop, a radio being removed).
//
//   ClipWriter (the PASS thread pushes, ONE WORKER thread services): push() is wait-free into a
//   fixed 32-slot single-producer single-consumer queue; service(ring) is the worker's call, every
//   50-100 ms: it creates files, copies audio from the ring, closes files, appends index.csv. It
//   blocks on the disk and so must never run on the pass or the DSP thread. The counters are atomics
//   readable from any thread (the publish).
//
// WHAT A CLIP IS (the contract the tests pin).
//   * It starts preRollS before the pass that saw the gate open (clamped at the first sample the ring
//     ever held) and ends tailS after the pass that saw the gate close. A gate that opens again inside the tail
//     does NOT end the clip: the clip simply goes on (hang time).
//   * Its length for the MINIMUM test is the transmission itself: first gate-open to last gate-close,
//     excluding pre-roll and tail. Shorter than minS: the file is deleted (a click is not a clip).
//     A transmission of exactly minS is kept.
//   * A gate that never closes ends a clip every maxClipS and starts the next at the same sample
//     (no audio lost between the two; the split clip has no pre-roll).
//   * Edges are as sharp as the pass (a few ms to tens of ms); the pre-roll default hides it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "core/recorder.hpp"
#include "core/replay_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace cascade::core {

// ---------------------------------------------------------------------------------------
// The commands the pass hands to the writer.
// ---------------------------------------------------------------------------------------

struct ClipCmd {
    enum class Kind : std::uint8_t { Open, Close, Drop };
    Kind kind = Kind::Open;
    std::uint32_t clipId = 0;
    // Open: first sample of the clip (pre-roll included). Close/Drop: one past the last sample.
    std::uint64_t sample = 0;
    // Open only: the sample at which the gate was seen to open (the transmission starts here).
    std::uint64_t gateSample = 0;
    std::int64_t unixTime = 0;  // Open only: UTC seconds at the gate opening
    double frequencyHz = 0.0;   // Open only
    char mode[8] = {};          // Open only, NUL-terminated ("NFM")
};

// ---------------------------------------------------------------------------------------
// The state machine (pass thread).
// ---------------------------------------------------------------------------------------

struct ClipSettings {
    double rateHz = 48000.0;
    double preRollS = 2.0;    // 0..10
    double tailS = 3.0;       // 0..30
    double minS = 1.0;        // 0..60
    double maxClipS = 600.0;  // 10..3600
};

class ClipTrigger {
public:
    // Applies the settings, clamping each to its range (a non-finite value takes the default). Safe between
    // clips; changing them while a clip is open takes effect from the next decision. Returns the clamped copy.
    ClipSettings configure(ClipSettings s) {
        const ClipSettings d;
        auto fix = [](double v, double lo, double hi, double dflt) {
            return std::isfinite(v) ? std::clamp(v, lo, hi) : dflt;
        };
        s.rateHz = (std::isfinite(s.rateHz) && s.rateHz >= 1.0) ? std::min(s.rateHz, 1.0e7) : d.rateHz;
        s.preRollS = fix(s.preRollS, 0.0, 10.0, d.preRollS);
        s.tailS = fix(s.tailS, 0.0, 30.0, d.tailS);
        s.minS = fix(s.minS, 0.0, 60.0, d.minS);
        s.maxClipS = fix(s.maxClipS, 10.0, 3600.0, d.maxClipS);
        set_ = s;
        return s;
    }
    const ClipSettings& settings() const { return set_; }

    // Forgets any open clip WITHOUT emitting anything (use endNow() first to close one properly).
    void reset() { state_ = State::Idle; }

    bool clipOpen() const { return state_ != State::Idle; }

    // Once a pass. Appends 0, 1 or 2 commands to out[0..cap) and returns how many (2 only when a clip is
    // split at maxClipS: Close then Open). With cap < 2 nothing is done and 0 is returned (the pass tries
    // again with room), so a caller must always give room for two.
    int update(bool gateOpen, std::uint64_t headSample, std::int64_t unixNow, double frequencyHz,
               const char* mode, ClipCmd* out, int cap) {
        if (cap < 2 || out == nullptr) { return 0; }
        int n = 0;
        switch (state_) {
        case State::Idle:
            if (gateOpen) { n += open(headSample, unixNow, frequencyHz, mode, true, out + n); }
            break;
        case State::Open:
            if (!gateOpen) {
                fallSample_ = headSample;
                state_ = State::Hang;
                if (set_.tailS <= 0.0) { n += finishClip(headSample, out + n); }
            } else if (headSample - segmentStart_ >= samples(set_.maxClipS)) {
                n += splitClip(headSample, unixNow, out + n);
            }
            break;
        case State::Hang:
            if (gateOpen) {
                state_ = State::Open;  // hang time: the same clip goes on
                if (headSample - segmentStart_ >= samples(set_.maxClipS)) { n += splitClip(headSample, unixNow, out + n); }
            } else if (headSample - fallSample_ >= samples(set_.tailS)) {
                n += finishClip(fallSample_ + samples(set_.tailS), out + n);
            }
            break;
        }
        return n;
    }

    // A stop, or a radio being removed: ends an open clip NOW (no tail beyond what has elapsed). One
    // command at most (Close, or Drop if the transmission was shorter than the minimum). Returns the count.
    int endNow(std::uint64_t headSample, ClipCmd* out, int cap) {
        if (cap < 1 || out == nullptr || state_ == State::Idle) { return 0; }
        std::uint64_t end = headSample;
        if (state_ == State::Open) {
            fallSample_ = headSample;
        } else {
            end = std::min<std::uint64_t>(headSample, fallSample_ + samples(set_.tailS));
        }
        return finishClip(end, out);
    }

private:
    enum class State : std::uint8_t { Idle, Open, Hang };

    std::uint64_t samples(double seconds) const {
        return static_cast<std::uint64_t>(std::llround(seconds * set_.rateHz));
    }

    int open(std::uint64_t head, std::int64_t unixNow, double freq, const char* mode, bool withPreRoll, ClipCmd* out) {
        const std::uint64_t pre = withPreRoll ? samples(set_.preRollS) : 0;
        ClipCmd c;
        c.kind = ClipCmd::Kind::Open;
        c.clipId = ++nextId_;
        c.sample = head > pre ? head - pre : 0;
        c.gateSample = head;
        c.unixTime = unixNow;
        c.frequencyHz = freq;
        if (mode != nullptr) { std::strncpy(c.mode, mode, sizeof c.mode - 1); }
        out[0] = c;
        state_ = State::Open;
        continuation_ = !withPreRoll;
        openSample_ = head;
        segmentStart_ = head;
        freq_ = freq;
        std::memcpy(mode_, c.mode, sizeof mode_);
        return 1;
    }

    int finishClip(std::uint64_t endSample, ClipCmd* out) {
        ClipCmd c;
        c.clipId = nextId_;
        c.sample = endSample;
        // The transmission: first gate-open of this clip to the last gate-close.
        // A continuation segment of a clip split at maxClipS is always kept: the transmission is long.
        const bool keep = continuation_ || (fallSample_ - openSample_) >= samples(set_.minS);
        c.kind = keep ? ClipCmd::Kind::Close : ClipCmd::Kind::Drop;
        out[0] = c;
        state_ = State::Idle;
        return 1;
    }

    // maxClipS reached with the gate still open: close here and continue at once from the same sample.
    int splitClip(std::uint64_t head, std::int64_t unixNow, ClipCmd* out) {
        ClipCmd close;
        close.kind = ClipCmd::Kind::Close;  // a split segment is long by construction (>= 10 s): always kept
        close.clipId = nextId_;
        close.sample = head;
        out[0] = close;
        const std::uint64_t keepOpenSample = head;
        const int m = open(head, unixNow, freq_, mode_, false, out + 1);
        openSample_ = keepOpenSample;
        return 1 + m;
    }

    ClipSettings set_;
    State state_ = State::Idle;
    std::uint32_t nextId_ = 0;
    std::uint64_t openSample_ = 0, fallSample_ = 0, segmentStart_ = 0;
    double freq_ = 0.0;
    char mode_[8] = {};
    bool continuation_ = false;
};

// ---------------------------------------------------------------------------------------
// The file writer (worker thread).
// ---------------------------------------------------------------------------------------

// What the worker reports about each clip it finished with.
struct ClipEvent {
    enum class Kind : std::uint8_t { Saved, Dropped, Failed };
    Kind kind = Kind::Saved;
    std::string path;
    std::string error;       // Failed (could not open) and any write trouble
    double durationS = 0.0;  // audio in the file (pre-roll and tail included)
    double peakDbfs = -200.0;
    std::int64_t startUnix = 0;
    bool gap = false;         // audio was lost: the worker fell more than the ring's length behind
    bool writeFailed = false; // the disk refused writes during the clip (full, removed, read-only)
};

class ClipWriter {
public:
    struct Config {
        std::string directory;          // where clips go; created on the first clip ("<record folder>/clips")
        std::string prefix = "radio1_clip";  // file name prefix; the name is <prefix>_YYYYMMDD_HHMMSS_<freq-kHz>.wav
        int radio = 1;                  // written to index.csv
        double rateHz = 48000.0;        // the ring's rate
        bool utcNames = false;          // names in UTC (tests) rather than local time (as the manual takes)
    };

    explicit ClipWriter(Config c = {}) : cfg_(std::move(c)) {}
    ~ClipWriter() { closeNow(); }
    ClipWriter(const ClipWriter&) = delete;
    ClipWriter& operator=(const ClipWriter&) = delete;

    // Worker or control thread, between clips (a clip already open keeps the folder it began in).
    void setDirectory(std::string dir) { cfg_.directory = std::move(dir); }
    const Config& config() const { return cfg_; }

    // TEST SEAM: replaces the file opener (core::Recorder::openFile otherwise), so a test can hand the writer a
    // file that refuses writes. Never set by the application.
    void setOpenerForTest(Recorder::Opener opener) { opener_ = std::move(opener); }

    // --- PASS thread ------------------------------------------------------------------

    // Wait-free. False (and the command counted in commandsLost()) when the queue is full, which only
    // happens when the worker has not run for 32 commands.
    bool push(const ClipCmd& c) {
        const std::size_t h = qHead_.load(std::memory_order_relaxed);
        const std::size_t t = qTail_.load(std::memory_order_acquire);
        if (h - t >= kQueue) {
            commandsLost_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_[h % kQueue] = c;
        qHead_.store(h + 1, std::memory_order_release);
        return true;
    }

    // --- WORKER thread ------------------------------------------------------------------

    // Processes queued commands and copies whatever new audio the ring holds into the open clip. Blocks on
    // the disk. Events for clips that finished are appended to `events` (may be null). Returns the number of
    // events appended.
    int service(const ReplayRing& ring, std::vector<ClipEvent>* events = nullptr) {
        int produced = 0;
        for (;;) {
            const std::size_t t = qTail_.load(std::memory_order_relaxed);
            if (t == qHead_.load(std::memory_order_acquire)) { break; }
            const ClipCmd c = queue_[t % kQueue];
            qTail_.store(t + 1, std::memory_order_release);
            produced += handle(c, ring, events);
        }
        produced += drain(ring, events);
        return produced;
    }

    // A stop or a radio removal, on the worker: finishes the open clip at once with whatever the ring holds
    // up to its current end (the last Close/Drop already queued, else the ring's head), closing the file.
    int finishAll(const ReplayRing& ring, std::vector<ClipEvent>* events = nullptr) {
        int produced = service(ring, events);
        if (open_) {
            if (!haveEnd_) { endSample_ = ring.head(); haveEnd_ = true; keep_ = true; }
            produced += drain(ring, events);
            if (open_) { produced += finishFile(events); }  // ring could not deliver the rest: close what there is
        }
        return produced;
    }

    // --- any thread ------------------------------------------------------------------

    bool clipOpen() const { return clipOpenPub_.load(std::memory_order_relaxed); }
    int clipsSaved() const { return saved_.load(std::memory_order_relaxed); }
    int clipsDropped() const { return dropped_.load(std::memory_order_relaxed); }
    int clipsFailed() const { return failed_.load(std::memory_order_relaxed); }
    int commandsLost() const { return commandsLost_.load(std::memory_order_relaxed); }
    // The seconds of the last clip finished (saved), 0 before any.
    double lastClipS() const { return lastClipS_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kQueue = 32;
    static constexpr std::size_t kChunk = 4096;

    void closeNow() {
        if (open_) {
            rec_.stop();
            open_ = false;
        }
    }

    static std::tm tmOf(std::int64_t unix, bool utc) {
        const std::time_t t = static_cast<std::time_t>(unix);
        std::tm out{};
#ifdef _WIN32
        if (utc) { gmtime_s(&out, &t); } else { localtime_s(&out, &t); }
#else
        if (utc) { gmtime_r(&t, &out); } else { localtime_r(&t, &out); }
#endif
        return out;
    }

    int handle(const ClipCmd& c, const ReplayRing& ring, std::vector<ClipEvent>* events) {
        int produced = 0;
        switch (c.kind) {
        case ClipCmd::Kind::Open:
            if (open_) {  // the previous clip's Close never came (queue overflow): end it where this one begins
                if (!haveEnd_) { endSample_ = c.sample; haveEnd_ = true; keep_ = true; }
                produced += drain(ring, events);
                if (open_) { produced += finishFile(events); }
            }
            produced += openClip(c, ring, events);
            break;
        case ClipCmd::Kind::Close:
        case ClipCmd::Kind::Drop:
            if (open_ && c.clipId == clipId_) {
                endSample_ = c.sample;
                haveEnd_ = true;
                keep_ = (c.kind == ClipCmd::Kind::Close);
            }
            break;
        }
        return produced;
    }

    int openClip(const ClipCmd& c, const ReplayRing& ring, std::vector<ClipEvent>* events) {
        ClipEvent ev;
        ev.startUnix = c.unixTime;
        // The name: <prefix>_YYYYMMDD_HHMMSS_<freq-kHz>.wav
        const std::tm tmv = tmOf(c.unixTime, cfg_.utcNames);
        char stamp[64];
        std::snprintf(stamp, sizeof stamp, "_%04d%02d%02d_%02d%02d%02d_%lld.wav", tmv.tm_year + 1900, tmv.tm_mon + 1,
                      tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                      static_cast<long long>(std::llround(c.frequencyHz / 1000.0)));
        Recorder::OpenRequest req;
        std::string err;
        if (!rec_.prepare(RecordKind::Audio, cfg_.directory, cfg_.rateHz, cfg_.prefix, req, err)) {
            return fail(ev, "clip not started: " + err, events);
        }
        req.path = (std::filesystem::path(cfg_.directory) / (cfg_.prefix + stamp)).string();
        Recorder::OpenedFile opened;
        const bool opened_ok = opener_ ? opener_(req, opened, err) : Recorder::openFile(req, opened, err);
        if (!opened_ok) { return fail(ev, "clip file not created: " + err, events); }
        if (!rec_.begin(std::move(opened), err)) { return fail(ev, "clip not started: " + err, events); }
        open_ = true;
        clipOpenPub_.store(true, std::memory_order_relaxed);
        clipId_ = c.clipId;
        path_ = rec_.path();
        freq_ = c.frequencyHz;
        std::memcpy(mode_, c.mode, sizeof mode_);
        startUnix_ = c.unixTime;
        haveEnd_ = false;
        keep_ = true;
        gap_ = false;
        peak_ = 0;
        written_ = 0;
        cursor_ = std::max<std::uint64_t>(c.sample, ring.oldest());
        // Pre-roll from before the ring ever held anything is not a loss; audio the ring DID hold and has since
        // overwritten (the worker was late opening the clip) is.
        if (c.sample >= ring.sampleFloor() && c.sample < ring.oldest()) { gap_ = true; }
        (void)events;
        return 0;
    }

    int fail(ClipEvent& ev, const std::string& error, std::vector<ClipEvent>* events) {
        ev.kind = ClipEvent::Kind::Failed;
        ev.error = error;
        failed_.fetch_add(1, std::memory_order_relaxed);
        if (events != nullptr) { events->push_back(ev); }
        return 1;
    }

    // Copies new audio for the open clip: up to its end if known, else up to the ring's head. Finishes the
    // file when the end has been reached.
    int drain(const ReplayRing& ring, std::vector<ClipEvent>* events) {
        if (!open_) { return 0; }
        std::int16_t buf[kChunk];
        float fbuf[kChunk];
        for (;;) {
            const std::uint64_t head = ring.head();
            const std::uint64_t target = haveEnd_ ? std::min(endSample_, head) : head;
            if (cursor_ >= target) { break; }
            if (ring.capacity() == 0) {  // the ring was switched off under us: nothing more will come
                if (haveEnd_) { cursor_ = endSample_; gap_ = true; }
                break;
            }
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, target - cursor_));
            std::uint64_t first = cursor_;
            const std::size_t got = ring.copyRange(cursor_, want, buf, &first);
            if (first > cursor_) {  // the worker fell behind the ring: that audio is gone
                gap_ = true;
                cursor_ = std::min(first, target);
            }
            if (got == 0) {
                if (cursor_ >= head || first >= head) { break; }
                continue;  // skipped a lost stretch; try again from the first intact sample
            }
            for (std::size_t i = 0; i < got; ++i) {
                fbuf[i] = static_cast<float>(buf[i]) / 32768.0f;
                const int a = std::abs(static_cast<int>(buf[i]));
                if (a > peak_) { peak_ = a; }
            }
            rec_.writeAudio(fbuf, got);
            written_ += got;
            cursor_ += got;
        }
        if (haveEnd_ && cursor_ >= endSample_) { return finishFile(events); }
        if (!rec_.recording()) { return finishFile(events); }  // the take ended itself (size limit)
        return 0;
    }

    int finishFile(std::vector<ClipEvent>* events) {
        ClipEvent ev;
        ev.path = path_;
        ev.startUnix = startUnix_;
        ev.gap = gap_;
        ev.writeFailed = rec_.writeFailed();
        ev.durationS = static_cast<double>(written_) / cfg_.rateHz;
        ev.peakDbfs = peak_ > 0 ? 20.0 * std::log10(static_cast<double>(peak_) / 32768.0) : -200.0;
        rec_.stop();  // blocking: patches the header and closes
        open_ = false;
        clipOpenPub_.store(false, std::memory_order_relaxed);
        if (!keep_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            ev.kind = ClipEvent::Kind::Dropped;
            dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ev.kind = ClipEvent::Kind::Saved;
            if (ev.writeFailed) { ev.error = "the disk refused writes during this clip"; }
            saved_.fetch_add(1, std::memory_order_relaxed);
            lastClipS_.store(ev.durationS, std::memory_order_relaxed);
            appendIndex(ev, ev.error);
        }
        if (events != nullptr) { events->push_back(ev); }
        return 1;
    }

    // One line in <directory>/index.csv: start UTC, radio, frequency, mode, duration, peak level, gap. A failure to
    // write it is not an error of the clip.
    void appendIndex(ClipEvent& ev, std::string& error) {
        const std::filesystem::path idx = std::filesystem::path(cfg_.directory) / "index.csv";
        std::error_code ec;
        const bool exists = std::filesystem::exists(idx, ec) && std::filesystem::file_size(idx, ec) > 0;
        std::FILE* f = std::fopen(idx.string().c_str(), "ab");
        if (f == nullptr) {
            if (error.empty()) { error = "index.csv could not be written"; }
            return;
        }
        if (!exists) { std::fputs("start_utc,radio,frequency_hz,mode,duration_s,peak_dbfs,gap\n", f); }
        const std::tm u = tmOf(ev.startUnix, true);
        std::fprintf(f, "%04d-%02d-%02dT%02d:%02d:%02dZ,%d,%.0f,%s,%.3f,%.1f,%s\n", u.tm_year + 1900, u.tm_mon + 1,
                     u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, cfg_.radio, freq_, mode_, ev.durationS, ev.peakDbfs,
                     ev.gap ? "gap" : "");
        if (std::fclose(f) != 0 && error.empty()) { error = "index.csv could not be written"; }
    }

    Config cfg_;
    Recorder rec_;
    Recorder::Opener opener_;
    // The queue.
    std::array<ClipCmd, kQueue> queue_{};
    std::atomic<std::size_t> qHead_{0}, qTail_{0};
    // The open clip (worker thread only).
    bool open_ = false, haveEnd_ = false, keep_ = true, gap_ = false;
    std::uint32_t clipId_ = 0;
    std::uint64_t cursor_ = 0, endSample_ = 0, written_ = 0;
    int peak_ = 0;
    std::string path_;
    double freq_ = 0.0;
    char mode_[8] = {};
    std::int64_t startUnix_ = 0;
    // Published.
    std::atomic<bool> clipOpenPub_{false};
    std::atomic<int> saved_{0}, dropped_{0}, failed_{0}, commandsLost_{0};
    std::atomic<double> lastClipS_{0.0};
};

}  // namespace cascade::core
