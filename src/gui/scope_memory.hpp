// scope_memory.hpp - what the demod scope remembers between frames when its
// display is set to AVG or PERSIST (0.99.26). See ScopeDisplay in
// gui/demod_scope.hpp for what the three positions mean to a user.
//
// PURE AND FRAME-RATE INDEPENDENT. Every blend and every decay is driven by
// the time since the last frame, not by a count of frames, so a scope on a
// 144 Hz monitor and one on a laptop at 30 fps average over the same half
// second and fade over the same second and a half. A frame count would make
// the setting mean something different on every machine.
//
// A MEMORY BELONGS TO ONE AXIS. Each slot remembers a `key` the caller builds
// from whatever decides the axis (the input, the time base, the rate, the
// number of points); when it changes, the slot starts again from the new
// trace. Averaging a 10 ms sweep into a 20 ms one, or holding a peak at the
// wrong frequency after a switch to the multiplex, would draw a picture of
// nothing that was ever on the air.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_SCOPE_MEMORY_HPP
#define CASCADE_GUI_SCOPE_MEMORY_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace cascade::gui {

// AVG: an exponential average with this time constant. Half a second is long
// enough to steady a spectrum of programme audio and short enough that a
// retune shows within a breath.
inline constexpr double kScopeAverageTauS = 0.5;
// PERSIST: how long a mark takes to fade away entirely.
inline constexpr double kScopePersistS = 1.5;
// PERSIST on a spectrum: the held peak falls this fast. The tube is ruled
// 80 dB deep, so a peak from full scale is gone in two seconds.
inline constexpr float kScopePersistDbPerS = 40.0f;

// How much of the new trace to take this frame. NaN or non-positive `dtS`
// takes none (nothing has elapsed); a non-positive time constant takes all.
inline double scopeBlendAlpha(double dtS, double tauS) {
    if (!(dtS > 0.0)) { return 0.0; }
    if (!(tauS > 0.0)) { return 1.0; }
    return 1.0 - std::exp(-dtS / tauS);
}

// The key for one axis, from the numbers that decide it. FNV-1a over the
// values; collisions only cost an average that fails to reset, never a crash.
inline std::uint64_t scopeMemoryKey(std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0,
                                    std::uint64_t d = 0) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint64_t v : {a, b, c, d}) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

class ScopeMemory {
public:
    // One slot per trace that can be remembered at once. I and Q are separate
    // because the baseband position draws both.
    enum Slot : int { kAudio = 0, kI, kQ, kSpectrum, kSlots };

    void reset() {
        for (Trace& t : slots_) { t = Trace{}; }
        ghosts_.clear();
        ghostKey_ = 0;
    }

    // AVG. Blends `x` (n values) into the slot's running average and returns
    // it. The first trace on a new key IS the average - starting from zeros
    // would show a trace growing out of the axis for the first half second.
    // A non-finite sample neither enters the average nor spoils it.
    const float* average(int slot, const float* x, std::size_t n, double dtS,
                         std::uint64_t key) {
        Trace& t = at(slot);
        if (x == nullptr || n == 0) { return nullptr; }
        if (!t.valid || t.key != key || t.a.size() != n) {
            t.key = key;
            t.valid = true;
            t.a.assign(x, x + n);
            return t.a.data();
        }
        const float al = static_cast<float>(scopeBlendAlpha(dtS, kScopeAverageTauS));
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(x[i])) { continue; }
            if (!std::isfinite(t.a[i])) {
                t.a[i] = x[i];
                continue;
            }
            t.a[i] += al * (x[i] - t.a[i]);
        }
        return t.a.data();
    }

    // PERSIST on a spectrum: each bin holds its highest recent value, falling
    // kScopePersistDbPerS a second, and never below the live trace. Returns
    // the held line, which the face draws dim behind the live one.
    const float* persistPeak(int slot, const float* db, std::size_t n, double dtS,
                             std::uint64_t key) {
        Trace& t = at(slot);
        if (db == nullptr || n == 0) { return nullptr; }
        if (!t.valid || t.key != key || t.a.size() != n) {
            t.key = key;
            t.valid = true;
            t.a.assign(db, db + n);
            return t.a.data();
        }
        const float fall = (dtS > 0.0) ? kScopePersistDbPerS * static_cast<float>(dtS) : 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float held = std::isfinite(t.a[i]) ? t.a[i] - fall : db[i];
            t.a[i] = std::isfinite(db[i]) ? std::max(held, db[i]) : held;
        }
        return t.a.data();
    }

    // PERSIST on a waveform: a per-column envelope that takes in every
    // excursion at once and closes back onto the live trace over
    // kScopePersistS. `fullRange` is the tube's height in signal units, so the
    // closing speed is a fraction of the screen, whatever the gain. On return
    // memLo/memHi point at the remembered band (cols values each).
    bool persistEnvelope(int slot, const float* lo, const float* hi, std::size_t cols,
                         double dtS, float fullRange, std::uint64_t key, const float** memLo,
                         const float** memHi) {
        Trace& t = at(slot);
        if (lo == nullptr || hi == nullptr || cols == 0 || memLo == nullptr ||
            memHi == nullptr) {
            return false;
        }
        if (!t.valid || t.key != key || t.a.size() != cols || t.b.size() != cols) {
            t.key = key;
            t.valid = true;
            t.a.assign(lo, lo + cols);
            t.b.assign(hi, hi + cols);
        } else {
            const float close = (dtS > 0.0 && fullRange > 0.0f)
                                    ? fullRange * static_cast<float>(dtS / kScopePersistS)
                                    : 0.0f;
            for (std::size_t c = 0; c < cols; ++c) {
                // The band shrinks toward the live column, never past it.
                const float l = std::isfinite(t.a[c]) ? t.a[c] + close : lo[c];
                const float h = std::isfinite(t.b[c]) ? t.b[c] - close : hi[c];
                t.a[c] = std::min(l, lo[c]);
                t.b[c] = std::max(h, hi[c]);
            }
        }
        *memLo = t.a.data();
        *memHi = t.b.data();
        return true;
    }

    // PERSIST on the vector display: the figure as it was on recent frames.
    // Each is thinned to `maxPoints` and kept until it is kScopePersistS old.
    struct Ghost {
        std::vector<float> i;
        std::vector<float> q;
        double t = 0.0;
    };

    void pushVector(const float* i, const float* q, std::size_t n, double nowS,
                    std::uint64_t key, std::size_t maxPoints) {
        if (key != ghostKey_) {
            ghosts_.clear();
            ghostKey_ = key;
        }
        expire(nowS);
        if (i == nullptr || q == nullptr || n == 0 || maxPoints == 0) { return; }
        const std::size_t step = (n + maxPoints - 1) / maxPoints;
        Ghost g;
        g.t = nowS;
        for (std::size_t k = 0; k < n; k += (step > 0 ? step : 1)) {
            g.i.push_back(i[k]);
            g.q.push_back(q[k]);
        }
        ghosts_.push_back(std::move(g));
        // A hard cap as well as the age limit: a stalled clock must not let
        // the list grow without bound.
        while (ghosts_.size() > kMaxGhosts) { ghosts_.pop_front(); }
    }

    // The ghosts still within the persistence time at `nowS`, oldest first.
    const std::deque<Ghost>& ghosts(double nowS) {
        expire(nowS);
        return ghosts_;
    }

    // How bright a ghost of age `ageS` is drawn, 1 when new to 0 when gone.
    static float ghostWeight(double ageS) {
        if (!(ageS >= 0.0)) { return 1.0f; }
        const double w = 1.0 - ageS / kScopePersistS;
        return static_cast<float>(std::clamp(w, 0.0, 1.0));
    }

    static constexpr std::size_t kMaxGhosts = 240;

private:
    struct Trace {
        std::vector<float> a;
        std::vector<float> b;
        std::uint64_t key = 0;
        bool valid = false;
    };

    Trace& at(int slot) { return slots_[static_cast<std::size_t>(std::clamp(slot, 0, kSlots - 1))]; }

    void expire(double nowS) {
        while (!ghosts_.empty() && !(nowS - ghosts_.front().t < kScopePersistS)) {
            ghosts_.pop_front();
        }
    }

    Trace slots_[kSlots];
    std::deque<Ghost> ghosts_;
    std::uint64_t ghostKey_ = 0;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_SCOPE_MEMORY_HPP
