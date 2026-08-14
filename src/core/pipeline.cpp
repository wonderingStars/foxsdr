// Pipeline implementation. Pacing approach (documented per task):
//
// The source thread generates ~10 ms chunks and sleeps to ABSOLUTE deadlines
// (steady_clock, sleep_until, next += period) rather than sleeping a fixed
// duration per chunk. Windows' default timer granularity (~15.6 ms) makes any
// single sleep overshoot badly, but with absolute deadlines an overshoot is
// followed by zero-sleep catch-up chunks until the schedule is level again, so
// the *average* sample rate self-corrects to well within the ~20% tolerance.
// 10 ms chunks keep stop() latency low without busy-spinning. If the thread
// falls more than 250 ms behind (suspend, debugger, load spike) the schedule
// resyncs to "now" instead of bursting an unbounded backlog.
//
// SPDX-License-Identifier: MIT
#include "core/pipeline.hpp"

#include <chrono>

namespace cascade::core {

namespace {

// Ring sized for at least 4 source chunks (~40 ms) of headroom so a scheduler
// hiccup on the DSP side does not force sample drops, and at least 8 FFT
// blocks so accumulation never starves at low sample rates. Rounded up to a
// power of two because SpscRing requires it.
std::size_t ringCapacityFor(const Pipeline::Config& cfg) {
    double want = 4.0 * cfg.sampleRateHz * 0.010;
    const double blocks = 8.0 * static_cast<double>(cfg.fftSize);
    if (want < blocks) { want = blocks; }
    std::size_t cap = 1;
    while (static_cast<double>(cap) < want) { cap <<= 1; }
    return cap;
}

}  // namespace

Pipeline::Pipeline(Config cfg)
    : cfg_(cfg),
      sigGen_(cfg.sampleRateHz),
      ring_(ringCapacityFor(cfg)),
      estimator_(cfg.fftSize, cascade::dsp::WindowType::BlackmanHarris) {
    estimator_.setAlpha(cfg.averagingAlpha);
}

Pipeline::~Pipeline() {
    stop();
}

cascade::source::SigGen& Pipeline::sigGen() {
    return sigGen_;
}

void Pipeline::start() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    if (run_.load(std::memory_order_relaxed)) { return; }  // idempotent

    // A restart is a fresh acquisition: re-prime the average and discard
    // samples left in the ring from the previous run, so the first frames
    // reflect the current generator settings rather than stale history.
    // Draining here is safe — both threads are joined at this point.
    estimator_.reset();
    std::complex<float> scratch[256];
    while (ring_.read(scratch, 256) != 0) {}

    run_.store(true, std::memory_order_relaxed);
    srcThread_ = std::thread(&Pipeline::sourceThreadMain, this);
    dspThread_ = std::thread(&Pipeline::dspThreadMain, this);
}

void Pipeline::stop() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    run_.store(false, std::memory_order_relaxed);
    // Threads notice the flag within one sleep quantum (<= ~10 ms source,
    // ~1 ms DSP); joinable() makes a second stop() a no-op.
    if (srcThread_.joinable()) { srcThread_.join(); }
    if (dspThread_.joinable()) { dspThread_.join(); }
}

bool Pipeline::running() const {
    return run_.load(std::memory_order_relaxed);
}

bool Pipeline::getLatestFrame(SpectrumFrame& out) {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (latest_.seq <= out.seq) { return false; }
    out = latest_;
    return true;
}

void Pipeline::sourceThreadMain() {
    using clock = std::chrono::steady_clock;

    constexpr double kChunkSec = 0.010;
    std::size_t chunk = static_cast<std::size_t>(cfg_.sampleRateHz * kChunkSec + 0.5);
    if (chunk == 0) { chunk = 1; }
    std::vector<std::complex<float>> buf(chunk);

    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(kChunkSec));
    auto next = clock::now() + period;

    while (run_.load(std::memory_order_relaxed)) {
        sigGen_.generate(buf.data(), chunk);
        // A real-time source must not block: if the DSP side stalled and the
        // ring is full, the overflow is dropped (write() accepts what fits).
        ring_.write(buf.data(), chunk);

        std::this_thread::sleep_until(next);
        next += period;
        const auto now = clock::now();
        if (now - next > std::chrono::milliseconds(250)) {
            next = now;  // resync after a long stall instead of bursting
        }
    }
}

void Pipeline::dspThreadMain() {
    const std::size_t n = cfg_.fftSize;
    std::vector<std::complex<float>> acc(n);
    std::vector<float> db(n);
    std::size_t filled = 0;

    while (run_.load(std::memory_order_relaxed)) {
        filled += ring_.read(acc.data() + filled, n - filled);
        if (filled < n) {
            // Ring drained mid-block: yield instead of spinning. 1 ms is far
            // below the 10 ms production cadence, so this never limits the
            // frame rate; it just caps idle CPU burn.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        filled = 0;
        estimator_.process(acc.data(), db.data());

        std::lock_guard<std::mutex> lk(frameMutex_);
        latest_.dbBins = db;      // vector assignment reuses capacity after the first frame
        latest_.seq += 1;         // strictly increasing; never reset (see header)
    }
}

}  // namespace cascade::core
