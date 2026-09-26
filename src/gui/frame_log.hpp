// frame_log.hpp - FOXSDR_FRAME_LOG: every frame's start time and work time,
// kept in memory for the whole run and written out once, at exit.
//
// WHY. Frame time is one of the gates the engine extraction is judged by
// (docs/ENGINE-EXTRACTION.md section 3.1 in the foxsdr-api repository), and a
// frame time that is sampled, averaged on the fly or written while the run is
// still going measures the instrument as much as the application. So: the
// steady clock at the START of every frame, before event polling, into an
// array allocated before the first frame; the time from that start to the
// return of the buffer swap beside it; nothing written until the loop has
// ended. Frame time is then the interval between consecutive starts, which
// includes the swap and any stall - what a user sees - and the work time is
// there for diagnosis.
//
// FOXSDR_VSYNC_OFF=1 runs the swap unsynchronised, so that interval is the
// work rather than the display's refresh period. Both are debug switches for
// tools/measure_engine.ps1; with neither set the frame loop holds a null
// pointer and pays one test per frame. Unlike FOXSDR_MEASURE they are NOT
// limited to bounded --frames runs: they (and FOXSDR_FRAME_CAP_HZ below) work
// in an interactive session too, which is how a frame log of a real session
// by hand is taken.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cascade::gui {

class FrameLog {
public:
    // Null unless FOXSDR_FRAME_LOG names a file. FOXSDR_FRAME_LOG_CAP sets how
    // many frames are kept (default 1 << 20: over twelve minutes at 1400
    // frames a second, 16 MB); frames past it are counted, not stored.
    static std::unique_ptr<FrameLog> fromEnvironment() {
        const char* path = std::getenv("FOXSDR_FRAME_LOG");
        if (path == nullptr || *path == '\0') { return nullptr; }
        std::size_t cap = std::size_t{1} << 20;
        if (const char* c = std::getenv("FOXSDR_FRAME_LOG_CAP"); c != nullptr && *c != '\0') {
            const long long v = std::atoll(c);
            if (v > 0) { cap = static_cast<std::size_t>(v); }
        }
        return std::unique_ptr<FrameLog>(new FrameLog(path, cap));
    }

    // Whether the swap should run unsynchronised for a measurement.
    static bool vsyncOffRequested() {
        const char* v = std::getenv("FOXSDR_VSYNC_OFF");
        return v != nullptr && *v != '\0' && *v != '0';
    }

    // Top of the frame, before the events are polled.
    void frameStart() {
        if (count_ < cap_) { start_[count_] = nowNs(); }
    }

    // After the swap has returned. A frame that started past the capacity is
    // counted as overflow so the file says it is incomplete.
    void frameEnd() {
        if (count_ < cap_) {
            work_[count_] = nowNs() - start_[count_];
            ++count_;
        } else {
            ++overflow_;
        }
    }

    std::size_t frames() const { return count_; }

    // Once, after the loop. Plain text: a header, then one "start work" pair
    // of nanosecond integers per frame, the start on the steady clock.
    bool write() const {
        std::FILE* f = std::fopen(path_.c_str(), "wb");
        if (f == nullptr) { return false; }
        std::fprintf(f, "# foxsdr-frame-log/1 frames=%zu overflow=%zu\n", count_, overflow_);
        std::fprintf(f, "# start_ns work_ns\n");
        for (std::size_t i = 0; i < count_; ++i) {
            std::fprintf(f, "%lld %lld\n", static_cast<long long>(start_[i]),
                         static_cast<long long>(work_[i]));
        }
        const bool ok = std::fclose(f) == 0;
        return ok;
    }

    const std::string& path() const { return path_; }

private:
    FrameLog(std::string path, std::size_t cap)
        : path_(std::move(path)), cap_(cap), start_(cap, 0), work_(cap, 0) {}

    static std::int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    std::string path_;
    std::size_t cap_ = 0;
    std::size_t count_ = 0;
    std::size_t overflow_ = 0;
    // Allocated (and zeroed, so every page is touched) before the first frame.
    std::vector<std::int64_t> start_;
    std::vector<std::int64_t> work_;
};

// FOXSDR_FRAME_CAP_HZ=<n>: the frame loop paced to n frames a second by the
// clock, for the CPU measurement's "interface idle at a fixed rate".
//
// WHY, WHEN THE SWAP IS ALREADY SYNCHRONISED. On the development desktop the
// swap interval of 1 is not honoured - the driver presents unsynchronised and
// the loop renders ~1300 frames a second either way - so a CPU figure taken
// "with vsync on" measured the GUI thread rendering flat out, and moved with
// whatever the driver's own threads were doing (baseline runs of the same
// binary spread from 4 s to 27 s of CPU a minute). The measurement needs the
// frame rate a synchronised display would impose, so it imposes one: after
// the swap, the loop waits until 1/n s after the frame's start. A
// high-resolution waitable timer on Windows (the default timer tick is
// 15.6 ms, coarser than a 60 Hz frame); sleep_until elsewhere.
class FrameCap {
public:
    // Null unless FOXSDR_FRAME_CAP_HZ names a rate in (0, 1000].
    static std::unique_ptr<FrameCap> fromEnvironment();
    ~FrameCap();
    FrameCap(const FrameCap&) = delete;
    FrameCap& operator=(const FrameCap&) = delete;

    void frameStart();  // top of the frame
    void waitForNext();  // after the swap: until one period after frameStart

private:
    explicit FrameCap(double hz);
    std::int64_t periodNs_ = 0;
    std::int64_t startNs_ = 0;
    void* timer_ = nullptr;  // Windows waitable timer handle, or null
};

}  // namespace cascade::gui
