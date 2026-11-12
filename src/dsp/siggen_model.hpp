// siggen_model.hpp - the SIGNAL GENERATOR NODE as a model: a settings struct and a phase-continuous complex-baseband generator for tone, noise,
// sweep, AM and FM. Not wired (the node kind and the runner are forbidden files for this lane).
//
// WHAT EXISTS: source/siggen.hpp is the hardware-free radio's generator - tone slots, one noise floor and impulses, nothing that moves. Read first;
// this file does not touch it. What it adds is what a patch-canvas generator node needs to be a test instrument: a SWEEP, AM and FM, with a settings
// struct a node can carry and a settings text it can store. The two can share a node later (a tone here is the same tone slot there); nothing here
// depends on it.
//
// THE KINDS (all complex baseband, amplitude A = 10^(dBFS / 20), offsets in Hz from the radio's centre, phase continuous across calls AND across
// settings changes - a change moves the frequency, it never jumps the phase):
//   Tone   A exp(j 2 pi f t).
//   Noise  complex Gaussian, total power 10^(noiseDb / 10), from our own xoshiro256** and Box-Muller (so the same seed gives the same samples on
//          every compiler; std::normal_distribution does not promise that).
//   Sweep  a chirp from startHz to stopHz over periodS seconds. Linear: f(t) = f0 + (f1 - f0) t / T. Log: f(t) = f0 (f1 / f0)^(t / T), only when both
//          ends are non-zero and of one sign (otherwise it is linear). Direction Up repeats the ramp; UpDown goes there and back (a triangle).
//   AM     A (1 + m cos(2 pi fm t)) exp(j 2 pi fc t): double sideband WITH carrier, A is the CARRIER amplitude (the peak is A (1 + m)), depth m 0..1.
//          Each sideband sits at fc +- fm with amplitude A m / 2.
//   FM     A exp(j (2 pi fc t + beta sin(2 pi fm t))), beta = deviationHz / fm: the lines at fc + k fm have amplitude A |J_k(beta)| (Bessel functions
//          of the first kind), so beta = 2.4048 nulls the carrier.
// ON TOP OF ANY KIND a second noise floor (addNoiseDb, off at -300 or lower: exact zeros, not a tiny residue) can be mixed in.
//
// LIMITS (clamped, never refused): offsets within +-0.49 of the sample rate; amplitude -200 .. 0 dBFS; sweep period 1 ms .. 1 h; modulation
// frequency 0.01 Hz .. 0.45 of the sample rate; deviation up to 0.45 of the sample rate; non-finite numbers keep the previous value.
// No allocation in generate(); one thread (the settings are copied in at the start of a call by setSettings() from the same thread).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>

namespace cascade::dsp {

enum class GenKind : int { Off = 0, Tone = 1, Noise = 2, Sweep = 3, Am = 4, Fm = 5 };
enum class SweepShape : int { Linear = 0, Log = 1 };
enum class SweepDirection : int { Up = 0, UpDown = 1 };

inline const char* genKindName(GenKind k) {
    switch (k) {
        case GenKind::Tone: return "tone";
        case GenKind::Noise: return "noise";
        case GenKind::Sweep: return "sweep";
        case GenKind::Am: return "am";
        case GenKind::Fm: return "fm";
        default: return "off";
    }
}
inline GenKind genKindFromName(std::string_view s) {
    if (s == "tone") { return GenKind::Tone; }
    if (s == "noise") { return GenKind::Noise; }
    if (s == "sweep") { return GenKind::Sweep; }
    if (s == "am") { return GenKind::Am; }
    if (s == "fm") { return GenKind::Fm; }
    return GenKind::Off;
}

struct GenSettings {
    GenKind kind = GenKind::Tone;
    double freqHz = 1000.0;        // tone frequency / AM and FM carrier, offset from the centre
    double amplitudeDb = -20.0;    // dBFS
    double noiseDb = -40.0;        // the Noise kind's total power, dBFS
    double addNoiseDb = -300.0;    // a noise floor added to any kind (<= -300: none)
    double startHz = -10000.0;     // sweep
    double stopHz = 10000.0;
    double periodS = 1.0;
    SweepShape shape = SweepShape::Linear;
    SweepDirection direction = SweepDirection::Up;
    double modHz = 400.0;          // AM / FM modulating frequency
    double depth = 0.5;            // AM depth 0..1
    double deviationHz = 2500.0;   // FM peak deviation
    std::uint64_t seed = 1;
};

class SignalGenerator {
public:
    using Cf = std::complex<float>;

    explicit SignalGenerator(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 1000.0 ? sampleRateHz : 48000.0) {
        setSettings(GenSettings{});
    }

    double sampleRateHz() const { return fs_; }
    const GenSettings& settings() const { return s_; }

    // Takes new settings (each field clamped; a non-finite number keeps its old value). The phases and the sweep position carry on.
    void setSettings(const GenSettings& in) {
        GenSettings n = s_;
        n.kind = in.kind;
        n.shape = in.shape;
        n.direction = in.direction;
        auto take = [](double v, double old, double lo, double hi) { return std::isfinite(v) ? std::min(std::max(v, lo), hi) : old; };
        const double lim = 0.49 * fs_;
        n.freqHz = take(in.freqHz, n.freqHz, -lim, lim);
        n.amplitudeDb = take(in.amplitudeDb, n.amplitudeDb, -200.0, 0.0);
        n.noiseDb = take(in.noiseDb, n.noiseDb, -300.0, 10.0);
        n.addNoiseDb = take(in.addNoiseDb, n.addNoiseDb, -300.0, 10.0);
        n.startHz = take(in.startHz, n.startHz, -lim, lim);
        n.stopHz = take(in.stopHz, n.stopHz, -lim, lim);
        n.periodS = take(in.periodS, n.periodS, 0.001, 3600.0);
        n.modHz = take(in.modHz, n.modHz, 0.01, 0.45 * fs_);
        n.depth = take(in.depth, n.depth, 0.0, 1.0);
        n.deviationHz = take(in.deviationHz, n.deviationHz, 0.0, 0.45 * fs_);
        const bool reseed = (in.seed != s_.seed) || !seeded_;
        n.seed = in.seed;
        s_ = n;
        if (reseed) { seedRng(s_.seed); seeded_ = true; }
        amp_ = std::pow(10.0, s_.amplitudeDb / 20.0);
        noiseSigma_ = s_.noiseDb <= -300.0 ? 0.0 : std::sqrt(std::pow(10.0, s_.noiseDb / 10.0) / 2.0);
        addSigma_ = s_.addNoiseDb <= -300.0 ? 0.0 : std::sqrt(std::pow(10.0, s_.addNoiseDb / 10.0) / 2.0);
        logOk_ = s_.startHz != 0.0 && s_.stopHz != 0.0 && ((s_.startHz > 0.0) == (s_.stopHz > 0.0));
    }

    // Starts the phases and the sweep over (the noise stream is not rewound; give a new seed to do that).
    void reset() {
        phase_ = 0.0;
        modPhase_ = 0.0;
        sweepT_ = 0.0;
    }

    // The sweep's instantaneous frequency now (Hz), for a display of the node and for the tests.
    double sweepFrequencyNow() const { return sweepFrequency(sweepT_); }

    void generate(Cf* out, std::size_t n) {
        if (out == nullptr) { return; }
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        for (std::size_t i = 0; i < n; ++i) {
            double re = 0.0, im = 0.0;
            switch (s_.kind) {
                case GenKind::Off: break;
                case GenKind::Tone:
                    re = amp_ * std::cos(phase_);
                    im = amp_ * std::sin(phase_);
                    advance(kTwoPi * s_.freqHz / fs_);
                    break;
                case GenKind::Noise: {
                    double a, b;
                    gauss(&a, &b);
                    re = noiseSigma_ * a;
                    im = noiseSigma_ * b;
                    break;
                }
                case GenKind::Sweep:
                    re = amp_ * std::cos(phase_);
                    im = amp_ * std::sin(phase_);
                    advance(kTwoPi * sweepFrequency(sweepT_) / fs_);
                    sweepT_ += 1.0 / fs_;
                    if (sweepT_ >= (s_.direction == SweepDirection::UpDown ? 2.0 : 1.0) * s_.periodS) { sweepT_ -= (s_.direction == SweepDirection::UpDown ? 2.0 : 1.0) * s_.periodS; }
                    break;
                case GenKind::Am: {
                    const double env = amp_ * (1.0 + s_.depth * std::cos(modPhase_));
                    re = env * std::cos(phase_);
                    im = env * std::sin(phase_);
                    advance(kTwoPi * s_.freqHz / fs_);
                    advanceMod(kTwoPi * s_.modHz / fs_);
                    break;
                }
                case GenKind::Fm: {
                    const double beta = s_.deviationHz / s_.modHz;
                    const double ph = phase_ + beta * std::sin(modPhase_);
                    re = amp_ * std::cos(ph);
                    im = amp_ * std::sin(ph);
                    advance(kTwoPi * s_.freqHz / fs_);
                    advanceMod(kTwoPi * s_.modHz / fs_);
                    break;
                }
            }
            if (addSigma_ > 0.0) {
                double a, b;
                gauss(&a, &b);
                re += addSigma_ * a;
                im += addSigma_ * b;
            }
            out[i] = Cf(static_cast<float>(re), static_cast<float>(im));
        }
    }

    // ---- settings as text (one line, ASCII, locale-free enough for a config: numbers are written with six decimals) ----
    static std::string toText(const GenSettings& g) {
        auto num = [](double v) {
            char b[48];
            std::snprintf(b, sizeof b, "%.6f", v);
            return std::string(b);
        };
        std::string t = "FOXGEN1;";
        t += genKindName(g.kind);
        t += ';' + num(g.freqHz) + ';' + num(g.amplitudeDb) + ';' + num(g.noiseDb) + ';' + num(g.addNoiseDb) + ';' + num(g.startHz) + ';' + num(g.stopHz) + ';' +
             num(g.periodS) + ';' + std::to_string(static_cast<int>(g.shape)) + ';' + std::to_string(static_cast<int>(g.direction)) + ';' + num(g.modHz) + ';' +
             num(g.depth) + ';' + num(g.deviationHz) + ';' + std::to_string(g.seed);
        return t;
    }
    // False (and `out` untouched) for anything that is not exactly the form toText() writes.
    static bool fromText(std::string_view t, GenSettings* out) {
        if (out == nullptr || t.size() > 600) { return false; }
        constexpr std::string_view head = "FOXGEN1;";
        if (t.substr(0, head.size()) != head) { return false; }
        t.remove_prefix(head.size());
        std::string_view f[14];
        for (int i = 0; i < 14; ++i) {
            const std::size_t semi = t.find(';');
            if ((i < 13) != (semi != std::string_view::npos)) { return false; }  // exactly 14 fields
            f[i] = t.substr(0, semi);
            if (i < 13) { t.remove_prefix(semi + 1); }
        }
        GenSettings g;
        g.kind = genKindFromName(f[0]);
        if (g.kind == GenKind::Off && f[0] != "off") { return false; }
        double v[11];
        const int idx[11] = {1, 2, 3, 4, 5, 6, 7, 10, 11, 12, -1};
        for (int k = 0; k < 10; ++k) {
            if (!parseNum(f[idx[k]], &v[k])) { return false; }
        }
        g.freqHz = v[0]; g.amplitudeDb = v[1]; g.noiseDb = v[2]; g.addNoiseDb = v[3]; g.startHz = v[4]; g.stopHz = v[5]; g.periodS = v[6];
        g.modHz = v[7]; g.depth = v[8]; g.deviationHz = v[9];
        if (f[8] != "0" && f[8] != "1") { return false; }
        if (f[9] != "0" && f[9] != "1") { return false; }
        g.shape = f[8] == "1" ? SweepShape::Log : SweepShape::Linear;
        g.direction = f[9] == "1" ? SweepDirection::UpDown : SweepDirection::Up;
        if (f[13].empty() || f[13].size() > 20) { return false; }
        std::uint64_t seed = 0;
        for (char c : f[13]) {
            if (c < '0' || c > '9') { return false; }
            seed = seed * 10 + static_cast<std::uint64_t>(c - '0');
        }
        g.seed = seed;
        *out = g;
        return true;
    }

private:
    static bool parseNum(std::string_view s, double* v) {
        if (s.empty() || s.size() > 40) { return false; }
        std::size_t i = 0;
        bool neg = false;
        if (s[i] == '-') { neg = true; ++i; }
        if (i >= s.size()) { return false; }
        double ip = 0.0;
        std::size_t digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ip = ip * 10 + (s[i] - '0'); ++i; ++digits; }
        if (digits == 0) { return false; }
        double fp = 0.0, scale = 0.1;
        if (i < s.size() && s[i] == '.') {
            ++i;
            std::size_t fd = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') { fp += scale * (s[i] - '0'); scale *= 0.1; ++i; ++fd; }
            if (fd == 0) { return false; }
        }
        if (i != s.size()) { return false; }
        *v = neg ? -(ip + fp) : (ip + fp);
        return true;
    }

    double sweepFrequency(double t) const {
        const double T = s_.periodS;
        double u = t / T;  // 0 .. 1 (Up) or 0 .. 2 (UpDown)
        if (s_.direction == SweepDirection::UpDown) { u = u <= 1.0 ? u : 2.0 - u; }
        u = std::min(std::max(u, 0.0), 1.0);
        if (s_.shape == SweepShape::Log && logOk_) { return s_.startHz * std::pow(s_.stopHz / s_.startHz, u); }
        return s_.startHz + (s_.stopHz - s_.startHz) * u;
    }
    void advance(double inc) {
        phase_ += inc;
        if (phase_ > 1e6 || phase_ < -1e6) { phase_ = std::fmod(phase_, 6.283185307179586476925286766559); }
        else if (phase_ > 6.283185307179586) { phase_ -= 6.283185307179586 * std::floor(phase_ / 6.283185307179586); }
        else if (phase_ < -6.283185307179586) { phase_ += 6.283185307179586 * std::floor(-phase_ / 6.283185307179586); }
    }
    void advanceMod(double inc) {
        modPhase_ += inc;
        if (modPhase_ > 6.283185307179586) { modPhase_ -= 6.283185307179586 * std::floor(modPhase_ / 6.283185307179586); }
    }

    // xoshiro256** seeded by splitmix64
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    void seedRng(std::uint64_t seed) {
        for (auto& w : st_) {
            seed += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            w = z ^ (z >> 31);
        }
        haveSpare_ = false;
    }
    std::uint64_t next() {
        const std::uint64_t r = rotl(st_[1] * 5, 7) * 9;
        const std::uint64_t t = st_[1] << 17;
        st_[2] ^= st_[0];
        st_[3] ^= st_[1];
        st_[1] ^= st_[2];
        st_[0] ^= st_[3];
        st_[2] ^= t;
        st_[3] = rotl(st_[3], 45);
        return r;
    }
    double uniform() { return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }  // (0, 1)
    void gauss(double* a, double* b) {
        const double r = std::sqrt(-2.0 * std::log(uniform()));
        const double th = 6.283185307179586 * uniform();
        *a = r * std::cos(th);
        *b = r * std::sin(th);
    }

    double fs_;
    GenSettings s_{};
    double amp_ = 0.1, noiseSigma_ = 0.0, addSigma_ = 0.0;
    bool logOk_ = false, seeded_ = false, haveSpare_ = false;
    double phase_ = 0.0, modPhase_ = 0.0, sweepT_ = 0.0;
    std::uint64_t st_[4]{};
};

}  // namespace cascade::dsp
