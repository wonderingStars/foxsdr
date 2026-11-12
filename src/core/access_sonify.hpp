// access_sonify.hpp - sonification: signal strength, or the strongest signal in
// a spectrum, as a tone, for a user who cannot watch the meter or the
// waterfall. A pure tone-mapping block plus a tiny tone renderer used by the
// tests and available to the audio hand-off. No audio device is opened here.
//
//   level (dB)  --curve-->  t in [0,1]  --smoothing-->  pitch, beep rate, level
//   squelch closed / unknown level      -->  silence (the tone fades, never clicks)
//   tuning offset or spectrum position  -->  pan (equal-power)
//
// THE CURVE is user data: a piecewise-linear table of (dB, t) points, so a user
// who only wants the last 20 dB of a weak-signal band to move the pitch can say
// so. presets: linear(minDb, maxDb) and the default. dB below the first point
// give t of the first point, above the last the last (clamped, never
// extrapolated). NaN gives "unknown".
//
// PITCH is logarithmic by default (equal t steps are equal musical intervals,
// which is how a listener judges pitch) between minHz and maxHz; linear is an
// option. BEEP RATE is linear between minRate and maxRate (a Geiger-counter
// style readout: faster is stronger); rate 0 means a continuous tone.
//
// SMOOTHING is one-pole in the dB domain with separate attack (rising) and
// release (falling) time constants, stepped by the caller's elapsed time, so
// the result does not depend on the update rate. The first update after a
// reset snaps (no slide up from silence). The smoother KEEPS TRACKING while the
// squelch is closed: when it opens the tone starts at the current level, not at
// a stale one.
//
// SPECTRUM-PEAK VARIANT: strongest bin with parabolic interpolation (sub-bin
// position), level measured against the median of the bins (the noise floor,
// which a few signals do not move), position -> pitch across the span (so the
// pitch says WHERE the signal is), peak-over-floor dB -> t -> level/beep rate.
// No peak at least `minPeakOverFloorDb` above the floor means silence.
//
// All of it is arithmetic on floats; tests/test_access_sonify.cpp checks the
// numbers, including frequency measured on rendered samples and a peak found in
// a generated noisy tone at a fractional bin.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cascade::core::access {

struct CurvePoint { float db; float t; };

class LevelCurve {
public:
    LevelCurve() { setLinear(-120.0f, -20.0f); }

    // Replace the points. Points are sorted by dB; t is clamped to [0,1];
    // non-finite points are dropped. Fewer than two usable points (or all at
    // one dB) falls back to linear -120..-20, never to an empty curve.
    void setPoints(std::vector<CurvePoint> pts) {
        std::vector<CurvePoint> ok;
        for (const CurvePoint& p : pts) {
            if (std::isfinite(p.db) && std::isfinite(p.t)) { ok.push_back({p.db, std::clamp(p.t, 0.0f, 1.0f)}); }
        }
        std::sort(ok.begin(), ok.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.db < b.db; });
        // collapse equal dB (keep the last): a vertical step is not representable
        std::vector<CurvePoint> u;
        for (const CurvePoint& p : ok) {
            if (!u.empty() && u.back().db == p.db) { u.back() = p; } else { u.push_back(p); }
        }
        if (u.size() < 2) { setLinear(-120.0f, -20.0f); return; }
        pts_ = std::move(u);
    }
    void setLinear(float minDb, float maxDb) {
        if (!(std::isfinite(minDb) && std::isfinite(maxDb)) || !(maxDb > minDb)) { minDb = -120.0f; maxDb = -20.0f; }
        pts_ = {{minDb, 0.0f}, {maxDb, 1.0f}};
    }
    const std::vector<CurvePoint>& points() const { return pts_; }

    // NaN in -> NaN out.
    float map(float db) const {
        if (!std::isfinite(db)) { return std::numeric_limits<float>::quiet_NaN(); }
        if (db <= pts_.front().db) { return pts_.front().t; }
        if (db >= pts_.back().db) { return pts_.back().t; }
        for (std::size_t i = 1; i < pts_.size(); ++i) {
            if (db <= pts_[i].db) {
                const float f = (db - pts_[i - 1].db) / (pts_[i].db - pts_[i - 1].db);
                return pts_[i - 1].t + f * (pts_[i].t - pts_[i - 1].t);
            }
        }
        return pts_.back().t;
    }

private:
    std::vector<CurvePoint> pts_;
};

struct SonifySettings {
    LevelCurve curve;
    float minHz = 220.0f;
    float maxHz = 1760.0f;       // three octaves above minHz
    bool logPitch = true;
    float minRateHz = 0.0f;      // beeps per second at t = 0; 0 and maxRate 0 = continuous
    float maxRateHz = 0.0f;
    float duty = 0.5f;           // fraction of a beep period that is sounding
    float attackMs = 30.0f;
    float releaseMs = 250.0f;
    float panWidth = 1.0f;       // pan = clamp(offset * panWidth, -1, 1); 0 = centred
    float maxAmplitude = 0.3f;   // never full scale: this is for a person's ears
    bool muteWhenSquelchClosed = true;
    float silenceBelowT = 0.0f;  // t at or below this is silence (0 = only exactly t<=0 with curve floor)
};

// What the audio side needs, once per update.
struct ToneParams {
    bool audible = false;
    float freqHz = 0.0f;
    float amplitude = 0.0f;      // 0..maxAmplitude
    float pan = 0.0f;            // -1 left .. +1 right
    float beepRateHz = 0.0f;     // 0 = continuous
    float duty = 1.0f;
    float t = 0.0f;              // the smoothed curve position, for tests and displays
};

inline float pitchForT(const SonifySettings& s, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float lo = std::max(1.0f, s.minHz), hi = std::max(lo, s.maxHz);
    if (s.logPitch) { return lo * std::pow(hi / lo, t); }
    return lo + t * (hi - lo);
}
inline float rateForT(const SonifySettings& s, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return std::max(0.0f, s.minRateHz + t * (s.maxRateHz - s.minRateHz));
}

class Sonifier {
public:
    explicit Sonifier(SonifySettings s = SonifySettings()) : s_(std::move(s)) {}
    void setSettings(SonifySettings s) { s_ = std::move(s); }
    const SonifySettings& settings() const { return s_; }
    void reset() { have_ = false; }

    // levelDb: the level to follow (any dB scale: the curve is in the same one).
    // squelchOpen: false mutes. offset: -1..+1 position for pan (tuning offset
    // or position in the span), NaN = centre. dtMs: time since the last update.
    ToneParams update(float levelDb, bool squelchOpen, float offset, float dtMs) {
        ToneParams out;
        out.duty = s_.duty;
        const bool known = std::isfinite(levelDb);
        if (known) {
            if (!have_) { smoothedDb_ = levelDb; have_ = true; }
            else {
                const float tau = (levelDb > smoothedDb_) ? s_.attackMs : s_.releaseMs;
                const float dt = std::isfinite(dtMs) && dtMs > 0.0f ? dtMs : 0.0f;
                const float a = (tau <= 0.0f) ? 1.0f : 1.0f - std::exp(-dt / tau);
                smoothedDb_ += a * (levelDb - smoothedDb_);
            }
        }
        if (!have_) { return out; }      // nothing ever measured: silence
        const float t = s_.curve.map(smoothedDb_);
        out.t = t;
        out.pan = std::isfinite(offset) ? std::clamp(offset * s_.panWidth, -1.0f, 1.0f) : 0.0f;
        out.freqHz = pitchForT(s_, t);
        out.beepRateHz = rateForT(s_, t);
        const bool silentT = t <= s_.silenceBelowT;
        const bool muted = (!squelchOpen && s_.muteWhenSquelchClosed) || !known || silentT;
        out.audible = !muted;
        out.amplitude = muted ? 0.0f : s_.maxAmplitude * (0.25f + 0.75f * t);  // louder with strength, never inaudible when open
        return out;
    }

private:
    SonifySettings s_;
    bool have_ = false;
    float smoothedDb_ = 0.0f;
};

// --- spectrum peak -----------------------------------------------------------

struct SpectrumPeak {
    bool found = false;
    float position = 0.0f;       // 0..1 across the bins, sub-bin interpolated
    float peakDb = 0.0f;
    float floorDb = 0.0f;        // median of the bins
    float overFloorDb = 0.0f;
};

// `bins` are dB values (any reference), n >= 3. NaN bins are ignored for the
// maximum and the median. `minOverFloorDb`: a peak lower than that above the
// median is no peak. Noise-only bin power is exponentially distributed, so a
// bin exceeds x times the median with probability 2^-x; for 512 bins the
// strongest stands 14 dB (x = 25.1) over the median with probability about
// 512 * 2^-25.1 = 1.4e-5. 14 dB is therefore the default, and the test counts
// false peaks over 2000 noise-only frames.
inline SpectrumPeak findSpectrumPeak(const float* bins, std::size_t n, float minOverFloorDb) {
    SpectrumPeak r;
    if (bins == nullptr || n < 3) { return r; }
    std::vector<float> v;
    v.reserve(n);
    std::size_t best = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(bins[i])) { continue; }
        v.push_back(bins[i]);
        if (best == n || bins[i] > bins[best]) { best = i; }
    }
    if (v.size() < 3 || best == n) { return r; }
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    r.floorDb = v[v.size() / 2];
    r.peakDb = bins[best];
    r.overFloorDb = r.peakDb - r.floorDb;
    // Parabolic interpolation over the log magnitudes of the three bins around
    // the maximum (the standard three-point vertex), only with both neighbours.
    float frac = 0.0f;
    if (best > 0 && best + 1 < n && std::isfinite(bins[best - 1]) && std::isfinite(bins[best + 1])) {
        const float a = bins[best - 1], b = bins[best], c = bins[best + 1];
        const float den = a - 2.0f * b + c;
        if (den < 0.0f) { frac = std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f); }
    }
    r.position = std::clamp((static_cast<float>(best) + frac) / static_cast<float>(n - 1), 0.0f, 1.0f);
    r.found = r.overFloorDb >= minOverFloorDb;
    return r;
}

struct SpectrumSonifySettings {
    float minOverFloorDb = 14.0f;
    float maxOverFloorDb = 60.0f;    // t = 1 at this much over the floor
    float minHz = 220.0f;            // pitch across the span: left edge ...
    float maxHz = 1760.0f;           // ... to right edge
    bool logPitch = true;
    float minRateHz = 0.0f, maxRateHz = 0.0f, duty = 0.5f;
    float maxAmplitude = 0.3f;
};

// One update from a frame of bins. The pitch carries POSITION, the level and
// the beep rate carry STRENGTH over the floor. Pan follows position too.
inline ToneParams sonifySpectrum(const float* bins, std::size_t n, const SpectrumSonifySettings& s) {
    ToneParams out;
    out.duty = s.duty;
    const SpectrumPeak p = findSpectrumPeak(bins, n, s.minOverFloorDb);
    if (!p.found) { return out; }
    const float span = std::max(1.0f, s.maxOverFloorDb - s.minOverFloorDb);
    const float t = std::clamp((p.overFloorDb - s.minOverFloorDb) / span, 0.0f, 1.0f);
    const float lo = std::max(1.0f, s.minHz), hi = std::max(lo, s.maxHz);
    out.freqHz = s.logPitch ? lo * std::pow(hi / lo, p.position) : lo + p.position * (hi - lo);
    out.pan = std::clamp(p.position * 2.0f - 1.0f, -1.0f, 1.0f);
    out.beepRateHz = std::max(0.0f, s.minRateHz + t * (s.maxRateHz - s.minRateHz));
    out.amplitude = s.maxAmplitude * (0.25f + 0.75f * t);
    out.t = t;
    out.audible = true;
    return out;
}

// --- tone renderer -------------------------------------------------------------

// Phase-continuous sine with equal-power pan, a beep gate and a linear fade on
// every audibility change (5 ms) so that muting and unmuting never click.
class ToneRenderer {
public:
    explicit ToneRenderer(float sampleRate = 48000.0f) : sr_(sampleRate > 1000.0f ? sampleRate : 48000.0f) {}

    // Render `frames` interleaved stereo float samples for the given parameters.
    // The parameters hold for the whole block; call once per update.
    void render(const ToneParams& p, std::size_t frames, float* stereoOut) {
        const float targetGain = p.audible ? p.amplitude : 0.0f;
        const float fadeStep = 0.3f / (0.005f * sr_);   // gain change per sample: 0.3 (the default ceiling) in 5 ms
        const float panAngle = (std::clamp(p.pan, -1.0f, 1.0f) + 1.0f) * 0.78539816339f;  // 0..pi/2
        const float gl = std::cos(panAngle), gr = std::sin(panAngle);
        const double w = 2.0 * 3.14159265358979323846 * static_cast<double>(p.freqHz) / static_cast<double>(sr_);
        const bool beeping = p.beepRateHz > 0.0f;
        for (std::size_t i = 0; i < frames; ++i) {
            // gain ramp
            if (gain_ < targetGain) { gain_ = std::min(targetGain, gain_ + fadeStep); }
            else if (gain_ > targetGain) { gain_ = std::max(targetGain, gain_ - fadeStep); }
            float gate = 1.0f;
            if (beeping) {
                const float period = 1.0f / p.beepRateHz;
                gate = (std::fmod(beepClock_, period) < p.duty * period) ? 1.0f : 0.0f;
                beepClock_ += 1.0f / sr_;
                if (beepClock_ > 1.0e6f) { beepClock_ = 0.0f; }
            }
            // gate edges are smoothed by a one-pole so that a beep does not click either
            gateState_ += (gate - gateState_) * 0.01f;
            const float s = static_cast<float>(std::sin(phase_)) * gain_ * gateState_;
            phase_ += w;
            if (phase_ > 6.283185307179586) { phase_ -= 6.283185307179586; }
            stereoOut[2 * i] = s * gl;
            stereoOut[2 * i + 1] = s * gr;
        }
    }

private:
    float sr_;
    double phase_ = 0.0;
    float gain_ = 0.0f;
    float beepClock_ = 0.0f;
    float gateState_ = 1.0f;
};

}  // namespace cascade::core::access
