// SNR measurement tap: turns the channel-power readings the pipeline already makes into an SNR estimate with a history ring.
//
// WHAT IS REUSED (nothing re-implemented): the signal reading is Pipeline::signalPowerDb() (channel power INCLUDING the noise, dBFS);
// the noise floor is core::NoiseFloorTracker (noise_floor_tracker.hpp: the minimum of the last ten seconds of 100 ms block means, readings
// taken while the squelch gate is open are not fed, NaN until half a second is held); the SNR is powerlog::snrFromTotalAndFloor
// (power_log.hpp: (total - floor) / floor in the linear domain, NaN when the total is under 0.1 dB above the floor). S-UNIT conversion
// is NOT part of this tap: it already exists in core/smeter_cal.hpp (IARU Region 1 Technical Recommendation R.1: S9 = -73 dBm below
// 30 MHz, -93 dBm above, 6 dB per S unit; see that header for what was and was not verified) and is applied to dBm, not to SNR.
// WHAT THIS ADDS: one call per measurement (push), the latest sample, a lock-free ring of the last `capacity` samples that another
// thread (the screen) can copy without stopping the producer, and a mean over the last N seconds.
//
// THREADS: push() from ONE producer thread (the engine pass, not the audio callback: NoiseFloorTracker keeps a std::deque, which
// allocates a block now and then, about once per 100 ms-block, measured in the test). latest(), history() and meanSnrDb() from any
// thread: the ring is read with a sequence check, so a copy is either whole or retried (it never mixes two samples of different
// ages); a reader that is lapped by the producer gets fewer samples, never garbage. Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/noise_floor_tracker.hpp"
#include "core/power_log.hpp"

namespace cascade::core::meas {

struct SnrSample {
    double timeS = 0.0;
    double totalDb = 0.0;
    double floorDb = std::numeric_limits<double>::quiet_NaN();
    double snrDb = std::numeric_limits<double>::quiet_NaN();
};

class SnrTap {
public:
    explicit SnrTap(std::size_t capacity = 1024) {
        std::size_t c = 8;
        while (c < capacity && c < (1u << 20)) { c <<= 1; }
        ring_.resize(c);
        mask_ = c - 1;
    }

    std::size_t capacity() const { return ring_.size(); }

    // totalDb: channel power including noise, dBFS; nowS: any monotonic clock, seconds; gateOpen: the squelch gate state.
    void push(double totalDb, double nowS, bool gateOpen = false) {
        floor_.push(totalDb, nowS, gateOpen);
        SnrSample s;
        s.timeS = nowS;
        s.totalDb = totalDb;
        s.floorDb = floor_.floorDb();
        s.snrDb = powerlog::snrFromTotalAndFloor(totalDb, s.floorDb);
        const std::uint64_t n = count_.load(std::memory_order_relaxed);
        ring_[n & mask_] = s;
        count_.store(n + 1, std::memory_order_release);
    }

    std::uint64_t count() const { return count_.load(std::memory_order_acquire); }

    // The newest sample; false when nothing has been pushed.
    bool latest(SnrSample& out) const {
        for (int tries = 0; tries < 8; ++tries) {
            const std::uint64_t n = count_.load(std::memory_order_acquire);
            if (n == 0) { return false; }
            out = ring_[(n - 1) & mask_];
            if (count_.load(std::memory_order_acquire) - (n - 1) < ring_.size()) { return true; }
        }
        return false;
    }

    // Up to `maxOut` of the newest samples, OLDEST FIRST, into `out`; returns how many.
    std::size_t history(SnrSample* out, std::size_t maxOut) const {
        for (int tries = 0; tries < 8; ++tries) {
            const std::uint64_t n = count_.load(std::memory_order_acquire);
            const std::size_t have = static_cast<std::size_t>(std::min<std::uint64_t>(n, ring_.size() - 1));  // one slot stays unread: the one being written
            const std::size_t take = std::min(have, maxOut);
            const std::uint64_t first = n - take;
            for (std::size_t i = 0; i < take; ++i) { out[i] = ring_[(first + i) & mask_]; }
            // Valid only if the producer has not lapped the oldest slot we copied.
            if (count_.load(std::memory_order_acquire) - first < ring_.size()) { return take; }
        }
        return 0;
    }

    // Mean SNR (dB) of the valid samples in the last `seconds` (relative to the newest sample's time); NaN when there are none.
    double meanSnrDb(double seconds) const {
        std::vector<SnrSample> tmp(ring_.size());
        const std::size_t n = history(tmp.data(), tmp.size());
        if (n == 0) { return std::numeric_limits<double>::quiet_NaN(); }
        const double t1 = tmp[n - 1].timeS;
        double sum = 0.0;
        int cnt = 0;
        for (std::size_t i = n; i-- > 0;) {
            if (t1 - tmp[i].timeS > seconds) { break; }
            if (std::isfinite(tmp[i].snrDb)) {
                sum += tmp[i].snrDb;
                ++cnt;
            }
        }
        return cnt ? sum / cnt : std::numeric_limits<double>::quiet_NaN();
    }

    // Producer thread only (a retune, a bandwidth or mode change: the floor is another floor).
    void reset() {
        floor_.reset();
    }

private:
    NoiseFloorTracker floor_;
    std::vector<SnrSample> ring_;
    std::size_t mask_ = 0;
    std::atomic<std::uint64_t> count_{0};
};

}  // namespace cascade::core::meas
