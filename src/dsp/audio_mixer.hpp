// audio_mixer.hpp - the patch canvas's AUDIO MIXER NODE: N audio inputs with a gain, a balance, mute and solo each, a master gain and an optional
// soft limiter, mixed to a stereo pair (and to mono for the canvas's mono audio ports). Audio fan-in is refused by PatchGraph::connect() because two
// streams into one input is "an argument about which one wins" (patch_graph.hpp, the comment above `mergeable`); a mixer node is the place where
// the merge is defined, which is what this file is. Not wired (the patch canvas, patch_graph.hpp and the runner are forbidden files for this
// lane): the hand-off in docs/feature-records/dspmodels.md says what the node kind, ports and runner call need.
//
// THE LAWS (stated, tested):
//   GAIN      per input, in dB, -90 .. +24; at or below -90 dB the input is silent. Master gain the same range.
//   MONO input (no right channel given): PAN, CONSTANT POWER by default. x = (balance + 1) * pi/4, gL = cos(x), gR = sin(x): gL^2 + gR^2 = 1 at every
//             position, so a source does not change loudness as it is moved; centre is -3.01 dB on each side, hard left is 1 / 0. PanLaw::Linear
//             is the other choice: gL = (1 - balance) / 2, gR = (1 + balance) / 2 (-6.02 dB at the centre).
//   STEREO input (a right channel given): BALANCE, LINEAR, UNITY AT THE CENTRE: gL = min(1, 1 - balance), gR = min(1, 1 + balance). The louder side
//             stays at the input's own level and the other falls to nothing at the extreme; the centre is untouched.
//   MUTE      the input is silent. SOLO: while any input is soloed, every input that is not soloed is silent (soloing two plays both).
//   MONO OUT  (L + R) / sqrt(2): a mono input at the centre of a constant-power pan comes out at exactly its own level.
//   LIMITER   off by default. Linear up to the threshold T (default 0.8 of full scale), above it y = sign * (T + (1 - T) tanh((|x| - T) / (1 - T))):
//             continuous with a continuous slope at T, never above 1, strictly increasing. With it off nothing is clipped here - a sum above 1 passes
//             on as it is (the sink decides), and clipCount() counts the output samples above 1 so a meter can say so.
//   CLICKS    every gain (input gain, balance, mute/solo, master) moves LINEARLY to its new value over 5 ms, per side, never in one step.
//   BAD DATA  a non-finite input sample reads as 0; a null input is silent; an input past the count is ignored; out-of-range settings are clamped.
// NO ALLOCATION: every table is a fixed array (kMaxInputs inputs). THREADS: setters from any thread (atomics, picked up at the start of the next
// process()); process() from the audio thread only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cascade::dsp {

enum class PanLaw : int { ConstantPower = 0, Linear = 1 };

class AudioMixer {
public:
    static constexpr int kMaxInputs = 16;
    static constexpr double kMinGainDb = -90.0;
    static constexpr double kMaxGainDb = 24.0;
    static constexpr double kRampSeconds = 0.005;

    explicit AudioMixer(int inputs = 2, double sampleRateHz = 48000.0)
        : n_(std::min(std::max(inputs, 1), kMaxInputs)), rampSamples_(std::max(1.0, kRampSeconds * (sampleRateHz > 1000.0 ? sampleRateHz : 48000.0))) {
        for (auto& v : gainDb_) { v.store(0.0f); }
        for (auto& v : balance_) { v.store(0.0f); }
        for (auto& v : mute_) { v.store(false); }
        for (auto& v : solo_) { v.store(false); }
    }

    int inputCount() const { return n_; }

    void setGainDb(int i, double db) {
        if (valid(i) && std::isfinite(db)) { gainDb_[idx(i)].store(static_cast<float>(std::min(std::max(db, kMinGainDb), kMaxGainDb))); }
    }
    void setBalance(int i, double b) {
        if (valid(i) && std::isfinite(b)) { balance_[idx(i)].store(static_cast<float>(std::min(std::max(b, -1.0), 1.0))); }
    }
    void setMute(int i, bool m) {
        if (valid(i)) { mute_[idx(i)].store(m); }
    }
    void setSolo(int i, bool s) {
        if (valid(i)) { solo_[idx(i)].store(s); }
    }
    void setMasterDb(double db) {
        if (std::isfinite(db)) { masterDb_.store(static_cast<float>(std::min(std::max(db, kMinGainDb), kMaxGainDb))); }
    }
    void setPanLaw(PanLaw l) { law_.store(static_cast<int>(l)); }
    void setLimiter(bool on) { limiter_.store(on); }
    void setLimiterThreshold(double t) {
        if (std::isfinite(t)) { limitT_.store(static_cast<float>(std::min(std::max(t, 0.1), 0.99))); }
    }

    // Output samples above 1 in magnitude since construction / resetClipCount() (counted before the limiter: what the sum would have been).
    std::uint64_t clipCount() const { return clips_.load(); }
    void resetClipCount() { clips_.store(0); }

    // Soft limiter curve, public so a meter or a test can draw it.
    static float limit(float x, float t) {
        const float a = std::fabs(x);
        if (a <= t) { return x; }
        const float y = t + (1.0f - t) * std::tanh((a - t) / (1.0f - t));
        return x < 0 ? -y : y;
    }

    // The per-side gains of one input as the laws above give them (used by process() and by the tests).
    static void sideGains(double balance, bool stereoInput, PanLaw law, double* gl, double* gr) {
        balance = std::min(std::max(balance, -1.0), 1.0);
        if (stereoInput) {
            *gl = std::min(1.0, 1.0 - balance);
            *gr = std::min(1.0, 1.0 + balance);
        } else if (law == PanLaw::ConstantPower) {
            const double x = (balance + 1.0) * 0.78539816339744830962;  // pi / 4
            *gl = std::cos(x);
            *gr = std::sin(x);
        } else {
            *gl = (1.0 - balance) / 2.0;
            *gr = (1.0 + balance) / 2.0;
        }
    }

    // Mixes n frames. inL[i] is input i's left (or only) channel, inR[i] its right channel or null for a mono input; either array may hold nulls
    // (silent). outL / outR must not alias an input. The arrays are indexed 0 .. inputCount()-1.
    void process(const float* const* inL, const float* const* inR, std::size_t n, float* outL, float* outR) {
        mix(inL, inR, n, outL, outR, limiter_.load(), limitT_.load());
    }

    // The same mix as a single mono channel: (L + R) / sqrt(2), then the limiter (on the downmix) when it is on. outMono must not alias an input.
    // Works in pieces of kScratch frames on the stack.
    void processMono(const float* const* inL, const float* const* inR, std::size_t n, float* outMono) {
        if (outMono == nullptr) { return; }
        constexpr std::size_t kScratch = 256;
        float l[kScratch], r[kScratch];
        const bool lim = limiter_.load();
        const float t = limitT_.load();
        std::array<const float*, kMaxInputs> pl{}, pr{};
        for (std::size_t base = 0; base < n; base += kScratch) {
            const std::size_t m = std::min(kScratch, n - base);
            for (int i = 0; i < n_; ++i) {
                pl[idx(i)] = (inL != nullptr && inL[i] != nullptr) ? inL[i] + base : nullptr;
                pr[idx(i)] = (inR != nullptr && inR[i] != nullptr) ? inR[i] + base : nullptr;
            }
            mix(pl.data(), pr.data(), m, l, r, false, t);
            for (std::size_t s = 0; s < m; ++s) {
                float v = (l[s] + r[s]) * 0.70710678118654752f;
                if (lim) { v = limit(v, t); }
                outMono[base + s] = v;
            }
        }
    }

private:
    void mix(const float* const* inL, const float* const* inR, std::size_t n, float* outL, float* outR, bool lim, float t) {
        if (outL == nullptr || outR == nullptr) { return; }
        std::array<double, kMaxInputs> tgtL{}, tgtR{};
        bool anySolo = false;
        for (int i = 0; i < n_; ++i) { anySolo = anySolo || solo_[idx(i)].load(); }
        const PanLaw law = static_cast<PanLaw>(law_.load());
        for (int i = 0; i < n_; ++i) {
            const bool stereo = inR != nullptr && inR[i] != nullptr;
            const bool silent = mute_[idx(i)].load() || (anySolo && !solo_[idx(i)].load()) || inL == nullptr || inL[i] == nullptr;
            const double gdb = gainDb_[idx(i)].load();
            const double lin = (silent || gdb <= kMinGainDb) ? 0.0 : std::pow(10.0, gdb / 20.0);
            double gl, gr;
            sideGains(balance_[idx(i)].load(), stereo, law, &gl, &gr);
            tgtL[idx(i)] = lin * gl;
            tgtR[idx(i)] = lin * gr;
        }
        const double mdb = masterDb_.load();
        const double masterTgt = mdb <= kMinGainDb ? 0.0 : std::pow(10.0, mdb / 20.0);
        if (!primed_) {  // the very first block starts at its settings (no ramp from silence)
            for (int i = 0; i < n_; ++i) { curL_[idx(i)] = tgtL[idx(i)]; curR_[idx(i)] = tgtR[idx(i)]; }
            masterCur_ = masterTgt;
            primed_ = true;
        }
        std::array<double, kMaxInputs> stepL{}, stepR{};
        for (int i = 0; i < n_; ++i) {
            stepL[idx(i)] = (tgtL[idx(i)] - curL_[idx(i)]) / rampSamples_;
            stepR[idx(i)] = (tgtR[idx(i)] - curR_[idx(i)]) / rampSamples_;
        }
        const double masterStep = (masterTgt - masterCur_) / rampSamples_;
        std::uint64_t clipped = 0;
        for (std::size_t s = 0; s < n; ++s) {
            double accL = 0.0, accR = 0.0;
            for (int i = 0; i < n_; ++i) {
                const std::size_t k = idx(i);
                // advance this input's gains one sample toward the targets (landing on them exactly)
                if (curL_[k] != tgtL[k]) { curL_[k] = reached(curL_[k], tgtL[k], stepL[k]) ? tgtL[k] : curL_[k] + stepL[k]; }
                if (curR_[k] != tgtR[k]) { curR_[k] = reached(curR_[k], tgtR[k], stepR[k]) ? tgtR[k] : curR_[k] + stepR[k]; }
                if (inL == nullptr || inL[i] == nullptr) { continue; }
                double l = static_cast<double>(inL[i][s]);
                if (!std::isfinite(l)) { l = 0.0; }
                double r = l;
                if (inR != nullptr && inR[i] != nullptr) {
                    r = static_cast<double>(inR[i][s]);
                    if (!std::isfinite(r)) { r = 0.0; }
                }
                accL += l * curL_[k];
                accR += r * curR_[k];
            }
            if (masterCur_ != masterTgt) { masterCur_ = reached(masterCur_, masterTgt, masterStep) ? masterTgt : masterCur_ + masterStep; }
            float l = static_cast<float>(accL * masterCur_), r = static_cast<float>(accR * masterCur_);
            if (std::fabs(l) > 1.0f || std::fabs(r) > 1.0f) { ++clipped; }
            if (lim) {
                l = limit(l, t);
                r = limit(r, t);
            }
            outL[s] = l;
            outR[s] = r;
        }
        if (clipped != 0) { clips_.fetch_add(clipped); }
    }

    static bool reached(double cur, double tgt, double step) { return step == 0.0 || (step > 0.0 ? cur + step >= tgt : cur + step <= tgt); }
    bool valid(int i) const { return i >= 0 && i < n_; }
    static std::size_t idx(int i) { return static_cast<std::size_t>(i); }

    int n_;
    double rampSamples_;
    std::array<std::atomic<float>, kMaxInputs> gainDb_{}, balance_{};
    std::array<std::atomic<bool>, kMaxInputs> mute_{}, solo_{};
    std::atomic<float> masterDb_{0.0f};
    std::atomic<int> law_{0};
    std::atomic<bool> limiter_{false};
    std::atomic<float> limitT_{0.8f};
    std::atomic<std::uint64_t> clips_{0};
    // audio-thread state
    bool primed_ = false;
    std::array<double, kMaxInputs> curL_{}, curR_{};
    double masterCur_ = 1.0;
};

}  // namespace cascade::dsp
