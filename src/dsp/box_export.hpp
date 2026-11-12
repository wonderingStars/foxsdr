// Export of a selected time-and-frequency box from a recorded I/Q file: the part of the recording inside the box, mixed to baseband, low-pass filtered
// and decimated, written as filtered I/Q (float32 WAV or raw cf32) or demodulated (AM, FM, USB, LSB) to a mono WAV.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). It reads through source/playback_transport.hpp's TransportSource (seek + read of
// complex frames), which a recorded-file reader adapts to; the screen supplies the box (analyser selection) and a path.
//
// WHAT HAPPENS TO THE SAMPLES. Box centre f0 = (lo + hi) / 2, width B = hi - lo (both clipped to the recording's band centre +- rate / 2; `clipped` says so).
//   1. every frame is rotated by exp(-j 2 pi (f0 - centre) n / rate) with the phase kept in double precision from the ABSOLUTE frame number n, so the box
//      centre lands on 0 Hz and the phase is the same wherever the export starts;
//   2. a linear-phase low-pass FIR (Kaiser window; stop-band attenuation `attenuationDb`, default 80 dB) removes everything outside +-B/2, and the signal
//      is decimated by the integer D = floor(rate / minOutRate) (at least 1), where minOutRate is the greatest of 1.25 B, the user's minimum, and for the
//      single-sideband modes 2.2 B (the real audio then reaches B Hz). The pass-band edge is B/2 and the stop-band edge is outRate - B/2, so anything that
//      would FOLD into the pass band by the decimation is already below the stop-band level;
//   3. the filter's delay is removed: output frame k is the filter centred on input frame s0 + k D, so the first output frame is at the box start time and
//      the box's own signal is not preceded by the filter's start-up (the filter reads (N-1)/2 frames before the box and after it; frames before the file
//      or after its end are zeros).
// Output rate = rate / D, which is not always an integer: the WAV header carries its nearest integer (an error under 0.5 Hz in rate / D) and
// BoxResult::outRateHz is the exact value.
// DEMODULATION (after step 3): AM = |x| minus a running mean (50 ms time constant); FM = the phase step between successive frames, scaled so a deviation of
// fmDeviationHz (default B / 2) is +-1.0; USB = real part of x rotated up by B / 2 (the box's LOW edge becomes 0 Hz and the audio runs 0..B); LSB = real part
// of x rotated down by B / 2 (the box's HIGH edge becomes 0 Hz). Gain `audioGain` applies last. USB / LSB / AM / FM audio is mono at the output rate.
// Pcm16 rounds half away from zero and clips at the rails; NaN reads as 0.
//
// LIMITS. An export that would pass 4 GiB is refused BEFORE it starts (the WAV header cannot say more): narrow the box. At most 8191 filter taps (a
// setting): the output rate is raised above 1.25 B, when needed, so that the transition band is wide enough for the asked attenuation within that many
// taps (a 500 Hz box in a 2.4 MS/s recording is written at about 2 kS/s, not 625 S/s). Only a box almost as wide as the recording at D = 1 can still run
// out of taps; BoxResult::designAttenuationDb always says what was achieved.
// A cancel flag is checked between blocks; a cancelled export leaves its partial file (a valid WAV of what was written).
//
// PRIVACY: no output, no logging; the only text is the error string, which names the problem and never a path.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "source/playback_transport.hpp"

namespace cascade::dsp::boxexport {

using cf = std::complex<float>;
using cascade::source::transport::TransportSource;

enum class Mode : int { Iq = 0, Am, Fm, Usb, Lsb };
enum class Format : int { WavFloat32 = 0, WavPcm16, RawCf32 };

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr unsigned kMaxTaps = 8191;
constexpr std::uint64_t kMaxFileBytes = 0xFFFFF000ull;      // under 4 GiB, header included

struct BoxSpec {
    double sampleRateHz = 0.0;           // of the recording
    double centerHz = 0.0;               // of the recording
    double t0 = 0.0, t1 = 0.0;           // seconds from the first frame
    double loHz = 0.0, hiHz = 0.0;       // absolute RF frequencies
    Mode mode = Mode::Iq;
    Format format = Format::WavFloat32;
    double attenuationDb = 80.0;         // stop-band target, 40 .. 120
    double minOutRateHz = 0.0;           // 0 = only what the box needs
    unsigned maxTaps = kMaxTaps;         // 31 .. 8191
    double audioGain = 1.0;
    double fmDeviationHz = 0.0;          // 0 = B / 2
};

struct BoxResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    double outRateHz = 0.0;
    unsigned decimation = 0;
    unsigned taps = 0;
    double designAttenuationDb = 0.0;    // what the taps count could reach
    double designRippleDb = 0.0;         // the Kaiser design's pass-band ripple bound, 20 log10(1 + delta)
    double boxLoHz = 0.0, boxHiHz = 0.0; // after clipping to the band
    double t0 = 0.0, t1 = 0.0;           // after clipping to the file
    bool clipped = false;
    std::uint64_t framesWritten = 0;
    float peak = 0.0f;                   // largest |sample| written (before PCM16 rounding)
};

// Where the samples go: `frames` frames of `channels` interleaved floats (2 for I/Q, 1 for audio). Returns false to stop (disk full).
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool begin(unsigned channels, double outRateHz, std::uint64_t expectedFrames) = 0;
    virtual bool write(const float* interleaved, std::size_t frames) = 0;
};

namespace detail {

inline double bessel0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-18 * sum) { break; }
    }
    return sum;
}

struct Design {
    std::vector<float> taps;             // odd length, symmetric, unit DC gain
    double attenuationDb = 0.0;
    double rippleDb = 0.0;
};

// Kaiser-window low-pass: pass-band edge `fpass`, stop-band edge `fstop` (Hz), sample rate `fs`, target attenuation `a` dB, at most `maxTaps` taps.
inline Design designLowpass(double fpass, double fstop, double fs, double a, unsigned maxTaps) {
    Design d;
    const double dw = 2.0 * kPi * (fstop - fpass) / fs;
    unsigned n = static_cast<unsigned>(std::ceil((a - 7.95) / (2.285 * dw))) + 1;
    if (n < 5) { n = 5; }
    if ((n & 1u) == 0) { ++n; }
    double achieved = a;
    if (n > maxTaps) {
        n = maxTaps | 1u;
        if (n > maxTaps) { n -= 2; }
        achieved = 2.285 * dw * (static_cast<double>(n) - 1.0) + 7.95;      // the attenuation these taps can reach (Kaiser's relation, inverted)
        achieved = std::min(achieved, a);
    }
    const double beta = achieved > 50.0 ? 0.1102 * (achieved - 8.7) : (achieved > 21.0 ? 0.5842 * std::pow(achieved - 21.0, 0.4) + 0.07886 * (achieved - 21.0) : 0.0);
    const double fc = (fpass + fstop) / 2.0 / fs;                                // cycles per sample
    d.taps.resize(n);
    std::vector<double> h(n);
    const double mid = (static_cast<double>(n) - 1.0) / 2.0;
    const double i0b = bessel0(beta);
    double sum = 0.0;
    for (unsigned i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) - mid;
        const double sinc = x == 0.0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
        const double r = x / mid;
        const double w = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h[i] = sinc * w;
        sum += h[i];
    }
    for (unsigned i = 0; i < n; ++i) { d.taps[i] = static_cast<float>(h[i] / sum); }
    d.attenuationDb = achieved;
    const double delta = std::pow(10.0, -achieved / 20.0);
    d.rippleDb = 20.0 * std::log10(1.0 + delta);
    return d;
}

inline std::int16_t toPcm16(float v) {
    if (!(v == v)) { return 0; }
    const double s = static_cast<double>(v) * 32767.0;
    const double c = std::min(32767.0, std::max(-32768.0, s));
    return static_cast<std::int16_t>(c >= 0 ? c + 0.5 : c - 0.5);
}

}  // namespace detail

// What the export of this spec would do, without reading a sample: validates the box against the recording and gives rate, decimation and taps. Used by
// exportBox() and by a screen that wants to show "20 kHz, 12.5 MB" before the user confirms.
struct Plan {
    bool ok = false;
    std::string error;
    BoxResult info;
    std::uint64_t s0 = 0;                // first frame of the box
    std::uint64_t outFrames = 0;
    std::uint64_t outBytes = 0;
    unsigned channels = 1;
    double step = 0.0;                   // box centre minus recording centre
    double bandwidth = 0.0;
    detail::Design design;
};

inline Plan planExport(std::uint64_t totalFrames, const BoxSpec& s) {
    Plan p;
    auto fail = [&](const char* m) { p.ok = false; p.error = m; return p; };
    if (!std::isfinite(s.sampleRateHz) || s.sampleRateHz < 1.0 || s.sampleRateHz > 1.0e10) { return fail("the recording's sample rate is not valid"); }
    if (!std::isfinite(s.centerHz) || std::fabs(s.centerHz) > 1.0e12) { return fail("the recording's centre frequency is not valid"); }
    if (!std::isfinite(s.t0) || !std::isfinite(s.t1) || !std::isfinite(s.loHz) || !std::isfinite(s.hiHz)) { return fail("the box has a value that is not a number"); }
    if (!(s.loHz < s.hiHz)) { return fail("the box's lower frequency is not below its upper frequency"); }
    if (!(s.t0 < s.t1)) { return fail("the box's start time is not before its end time"); }
    if (!std::isfinite(s.attenuationDb) || s.attenuationDb < 40.0 || s.attenuationDb > 120.0) { return fail("the attenuation must be between 40 and 120 dB"); }
    if (s.maxTaps < 31 || s.maxTaps > kMaxTaps) { return fail("the tap limit must be between 31 and 8191"); }
    if (!std::isfinite(s.minOutRateHz) || s.minOutRateHz < 0.0) { return fail("the minimum output rate is not valid"); }
    if (!std::isfinite(s.audioGain) || s.audioGain < 0.0 || s.audioGain > 1.0e6) { return fail("the audio gain is not valid"); }
    if (!std::isfinite(s.fmDeviationHz) || s.fmDeviationHz < 0.0) { return fail("the FM deviation is not valid"); }
    if (s.mode == Mode::Iq && s.format == Format::WavPcm16) { return fail("I/Q is written as float32 WAV or raw cf32, not PCM16"); }
    if (s.mode != Mode::Iq && s.format == Format::RawCf32) { return fail("audio is written as a WAV, not raw cf32"); }
    const double bandLo = s.centerHz - s.sampleRateHz / 2.0, bandHi = s.centerHz + s.sampleRateHz / 2.0;
    BoxResult& r = p.info;
    r.boxLoHz = std::max(s.loHz, bandLo);
    r.boxHiHz = std::min(s.hiHz, bandHi);
    if (!(r.boxLoHz < r.boxHiHz)) { return fail("the box is outside the recording's band"); }
    r.clipped = r.boxLoHz != s.loHz || r.boxHiHz != s.hiHz;
    const double dur = static_cast<double>(totalFrames) / s.sampleRateHz;
    r.t0 = std::max(0.0, s.t0);
    r.t1 = std::min(dur, s.t1);
    if (r.t0 != s.t0 || r.t1 != s.t1) { r.clipped = true; }
    if (!(r.t0 < r.t1)) { return fail("the box is outside the recording's time span"); }
    const double s0d = std::floor(r.t0 * s.sampleRateHz + 0.5), s1d = std::floor(r.t1 * s.sampleRateHz + 0.5);
    p.s0 = static_cast<std::uint64_t>(s0d);
    const std::uint64_t s1 = std::min<std::uint64_t>(totalFrames, static_cast<std::uint64_t>(s1d));
    if (s1 <= p.s0) { return fail("the box holds no frames"); }
    p.bandwidth = r.boxHiHz - r.boxLoHz;
    p.step = (r.boxLoHz + r.boxHiHz) / 2.0 - s.centerHz;
    const bool ssb = s.mode == Mode::Usb || s.mode == Mode::Lsb;
    double minOut = std::max(p.bandwidth * (ssb ? 2.2 : 1.25), s.minOutRateHz);
    // a narrow box in a wide recording would need more taps than allowed for the transition band between B and the output rate: keep that band wide
    // enough for the tap limit (Kaiser: taps = (A - 7.95) rate / (2.285 * 2 pi * transition)); the output is then slightly oversampled, not under-filtered
    minOut = std::max(minOut, p.bandwidth + 1.05 * (s.attenuationDb - 7.95) * s.sampleRateHz / (2.285 * 2.0 * kPi * static_cast<double>(s.maxTaps)));
    if (minOut > s.sampleRateHz) { minOut = s.sampleRateHz; }
    double dd = std::floor(s.sampleRateHz / minOut);
    if (dd < 1.0) { dd = 1.0; }
    if (dd > 1.0e7) { return fail("the box is too narrow for this recording's rate"); }
    r.decimation = static_cast<unsigned>(dd);
    r.outRateHz = s.sampleRateHz / dd;
    const double fstop = r.decimation == 1 ? s.sampleRateHz / 2.0 : r.outRateHz - p.bandwidth / 2.0;
    const double fpass = p.bandwidth / 2.0;
    if (fstop - fpass <= 1e-6 * s.sampleRateHz || fpass >= s.sampleRateHz / 2.0) {
        // the box is the whole band (or more): nothing to filter, mixing alone
        p.design.taps.assign(1, 1.0f);
        p.design.attenuationDb = 0.0;
        p.design.rippleDb = 0.0;
    } else {
        p.design = detail::designLowpass(fpass, fstop, s.sampleRateHz, s.attenuationDb, s.maxTaps);
    }
    r.taps = static_cast<unsigned>(p.design.taps.size());
    r.designAttenuationDb = p.design.attenuationDb;
    r.designRippleDb = p.design.rippleDb;
    const std::uint64_t span = s1 - p.s0;
    p.outFrames = (span + r.decimation - 1) / r.decimation;
    p.channels = s.mode == Mode::Iq ? 2u : 1u;
    const std::uint64_t bytesPerSample = (s.format == Format::WavPcm16) ? 2u : 4u;
    p.outBytes = p.outFrames * p.channels * bytesPerSample + 128;
    if (p.outBytes > kMaxFileBytes) { return fail("the export would be larger than 4 GiB: narrow the box"); }
    p.ok = true;
    p.info.ok = true;
    return p;
}

// Runs the export into `sink`. `cancel` and `progress` (fraction 0..1) are optional.
inline BoxResult exportBox(TransportSource& src, const BoxSpec& spec, Sink& sink, const std::atomic<bool>* cancel = nullptr,
                           const std::function<void(double)>& progress = nullptr) {
    const Plan plan = planExport(src.totalFrames(), spec);
    BoxResult res = plan.info;
    if (!plan.ok) { res.ok = false; res.error = plan.error; return res; }
    res.ok = false;
    const unsigned N = res.taps, D = res.decimation;
    const std::vector<float>& h = plan.design.taps;
    const std::uint64_t half = (N - 1) / 2;
    const std::uint64_t M = plan.outFrames;
    const std::uint64_t totalIn = (M - 1) * D + N;                       // frames fed to the filter, starting `half` frames before s0
    if (!sink.begin(plan.channels, res.outRateHz, M)) { res.error = "the output could not be started"; return res; }

    const double fs = spec.sampleRateHz;
    // mixer: phase from the absolute frame number
    const double stepCycles = -plan.step / fs;                           // cycles per frame (negative: shift down)
    auto cyclesAt = [&](std::int64_t n) { double c = std::fmod(stepCycles * static_cast<double>(n), 1.0); if (c < 0.0) { c += 1.0; } return c; };
    const std::int64_t firstIn = static_cast<std::int64_t>(plan.s0) - static_cast<std::int64_t>(half);
    const std::complex<double> rot(std::cos(2.0 * kPi * stepCycles), std::sin(2.0 * kPi * stepCycles));
    std::complex<double> osc(std::cos(2.0 * kPi * cyclesAt(firstIn)), std::sin(2.0 * kPi * cyclesAt(firstIn)));

    constexpr std::size_t kBlock = 65536;
    std::vector<cf> raw(kBlock);
    std::vector<cf> work(static_cast<std::size_t>(N - 1) + kBlock);      // filter history + the mixed block
    std::size_t have = 0;                                                // valid frames at the start of `work`
    std::vector<float> outBuf((kBlock / D + 2) * plan.channels);
    std::uint64_t consumed = 0;                                          // input frames fed so far
    std::uint64_t produced = 0;
    std::int64_t nextOut = static_cast<std::int64_t>(N) - 1;             // index (in the fed stream) of the frame that completes the next output
    bool srcOk = true;
    std::int64_t srcPos = -1;                                            // frame number of the next frame the source will deliver (-1 unknown)

    // demodulator state
    double dc = 0.0;
    bool dcInit = false;
    cf prev(0.0f, 0.0f);
    bool prevInit = false;
    std::complex<double> ssbOsc(1.0, 0.0);
    const double ssbCycles = (spec.mode == Mode::Usb ? 1.0 : -1.0) * (plan.bandwidth / 2.0) / res.outRateHz;
    const std::complex<double> ssbRot(std::cos(2.0 * kPi * ssbCycles), std::sin(2.0 * kPi * ssbCycles));
    const double amAlpha = 1.0 - std::exp(-1.0 / (0.05 * res.outRateHz));
    const double dev = spec.fmDeviationHz > 0.0 ? spec.fmDeviationHz : plan.bandwidth / 2.0;
    const double fmScale = res.outRateHz / (2.0 * kPi * dev);
    unsigned ssbCount = 0;
    float peak = 0.0f;

    while (consumed < totalIn) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) { res.cancelled = true; res.error = "cancelled"; res.framesWritten = produced; res.peak = peak; return res; }
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, totalIn - consumed));
        // fetch `want` frames starting at absolute frame firstIn + consumed (zeros outside the file)
        const std::int64_t at = firstIn + static_cast<std::int64_t>(consumed);
        for (std::size_t i = 0; i < want; ++i) { raw[i] = cf(0.0f, 0.0f); }
        {
            const std::int64_t total = static_cast<std::int64_t>(src.totalFrames());
            const std::int64_t from = std::max<std::int64_t>(at, 0);
            const std::int64_t to = std::min<std::int64_t>(at + static_cast<std::int64_t>(want), total);
            if (to > from) {
                if (srcPos != from) { if (!src.seek(static_cast<std::uint64_t>(from))) { srcOk = false; } srcPos = from; }
                std::size_t got = 0;
                const std::size_t need = static_cast<std::size_t>(to - from);
                while (srcOk && got < need) {
                    const std::size_t g = src.read(raw.data() + (from - at) + got, need - got);
                    if (g == 0) { break; }
                    got += g;
                }
                srcPos = from + static_cast<std::int64_t>(got);
                if (got < need) { srcOk = false; }
            }
        }
        if (!srcOk) { res.error = "the recording could not be read"; res.framesWritten = produced; res.peak = peak; return res; }
        // mix into the work buffer after the history
        for (std::size_t i = 0; i < want; ++i) {
            const std::complex<double> x(raw[i].real(), raw[i].imag());
            const std::complex<double> y = x * osc;
            work[have + i] = cf(static_cast<float>(y.real()), static_cast<float>(y.imag()));
            osc *= rot;
            if ((i & 4095u) == 4095u) { osc /= std::abs(osc); }
        }
        const std::size_t avail = have + want;
        // produce every output whose completing frame lies in this block
        std::size_t outCount = 0;
        const std::int64_t blockStart = static_cast<std::int64_t>(consumed) - static_cast<std::int64_t>(have);   // stream index of work[0]
        while (nextOut - blockStart < static_cast<std::int64_t>(avail) && produced + outCount < M) {
            const std::size_t last = static_cast<std::size_t>(nextOut - blockStart);       // work index of the newest frame in the window
            const std::size_t first = last + 1 - N;
            const cf* w = work.data() + first;
            double accR = 0.0, accI = 0.0;
            const unsigned mid = (N - 1) / 2;
            for (unsigned k = 0; k < mid; ++k) {                                            // symmetric taps: one multiply for two frames
                const double hk = h[k];
                accR += hk * (static_cast<double>(w[k].real()) + static_cast<double>(w[N - 1 - k].real()));
                accI += hk * (static_cast<double>(w[k].imag()) + static_cast<double>(w[N - 1 - k].imag()));
            }
            accR += static_cast<double>(h[mid]) * w[mid].real();
            accI += static_cast<double>(h[mid]) * w[mid].imag();
            nextOut += D;
            float* o = outBuf.data() + outCount * plan.channels;
            switch (spec.mode) {
                case Mode::Iq: o[0] = static_cast<float>(accR); o[1] = static_cast<float>(accI); break;
                case Mode::Am: {
                    const double m = std::hypot(accR, accI);
                    if (!dcInit) { dc = m; dcInit = true; }
                    dc += amAlpha * (m - dc);
                    o[0] = static_cast<float>((m - dc) * spec.audioGain);
                    break;
                }
                case Mode::Fm: {
                    const cf cur(static_cast<float>(accR), static_cast<float>(accI));
                    if (!prevInit) { prev = cur; prevInit = true; }
                    const std::complex<double> d = std::complex<double>(cur.real(), cur.imag()) * std::conj(std::complex<double>(prev.real(), prev.imag()));
                    o[0] = static_cast<float>(std::atan2(d.imag(), d.real()) * fmScale * spec.audioGain);
                    prev = cur;
                    break;
                }
                case Mode::Usb: case Mode::Lsb: {
                    const std::complex<double> y = std::complex<double>(accR, accI) * ssbOsc;
                    ssbOsc *= ssbRot;
                    if ((++ssbCount & 1023u) == 0) { ssbOsc /= std::abs(ssbOsc); }
                    o[0] = static_cast<float>(y.real() * spec.audioGain);
                    break;
                }
            }
            for (unsigned c = 0; c < plan.channels; ++c) {
                const float a = std::fabs(o[c]);
                if (a == a) { peak = std::max(peak, a); }
            }
            ++outCount;
        }
        if (outCount > 0) {
            if (!sink.write(outBuf.data(), outCount)) { res.error = "writing the output failed (disk full?)"; res.framesWritten = produced; res.peak = peak; return res; }
            produced += outCount;
        }
        // keep the last N-1 frames as history
        consumed += want;
        const std::size_t keep = std::min<std::size_t>(avail, N - 1);
        if (keep > 0) { std::memmove(work.data(), work.data() + (avail - keep), keep * sizeof(cf)); }
        have = keep;
        if (progress) { progress(static_cast<double>(consumed) / static_cast<double>(totalIn)); }
    }
    res.framesWritten = produced;
    res.peak = peak;
    if (produced != M) { res.error = "internal: the frame count is not what was planned"; return res; }
    res.ok = true;
    return res;
}

// ---- file sinks (our own WAV writer) -------------------------------------------------------------------------------------------------------

class FileSink final : public Sink {
public:
    FileSink(const std::filesystem::path& path, Format format) : path_(path), format_(format) {}
    ~FileSink() override { close(); }
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    bool begin(unsigned channels, double outRateHz, std::uint64_t) override {
        channels_ = channels;
#if defined(_WIN32)
        f_ = _wfopen(path_.c_str(), L"wb");
#else
        f_ = std::fopen(path_.string().c_str(), "wb");
#endif
        if (f_ == nullptr) { return false; }
        if (format_ == Format::RawCf32) { return true; }
        rate_ = static_cast<std::uint32_t>(std::floor(outRateHz + 0.5));
        if (rate_ == 0) { rate_ = 1; }
        const unsigned bits = format_ == Format::WavPcm16 ? 16 : 32;
        const unsigned tag = format_ == Format::WavPcm16 ? 1 : 3;
        unsigned char h[44];
        std::memcpy(h, "RIFF", 4); put32(h + 4, 0); std::memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
        put16(h + 20, tag); put16(h + 22, channels); put32(h + 24, rate_); put32(h + 28, rate_ * channels * bits / 8); put16(h + 32, channels * bits / 8); put16(h + 34, bits);
        std::memcpy(h + 36, "data", 4); put32(h + 40, 0);
        return std::fwrite(h, 1, 44, f_) == 44;
    }

    bool write(const float* data, std::size_t frames) override {
        if (f_ == nullptr) { return false; }
        const std::size_t n = frames * channels_;
        if (format_ == Format::WavPcm16) {
            buf16_.resize(n);
            for (std::size_t i = 0; i < n; ++i) { buf16_[i] = detail::toPcm16(data[i]); }
            if (std::fwrite(buf16_.data(), 2, n, f_) != n) { return false; }
            bytes_ += n * 2;
        } else {
            if (std::fwrite(data, 4, n, f_) != n) { return false; }
            bytes_ += n * 4;
        }
        return true;
    }

    // Patches the sizes and closes. The file is a valid WAV of what was written even when the export stopped early.
    bool close() {
        if (f_ == nullptr) { return true; }
        bool ok = true;
        if (format_ != Format::RawCf32) {
            unsigned char b[4];
            put32(b, static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes_, 0xFFFFFFFFull)));
            ok = ok && std::fseek(f_, 40, SEEK_SET) == 0 && std::fwrite(b, 1, 4, f_) == 4;
            put32(b, static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes_ + 36, 0xFFFFFFFFull)));
            ok = ok && std::fseek(f_, 4, SEEK_SET) == 0 && std::fwrite(b, 1, 4, f_) == 4;
        }
        ok = (std::fclose(f_) == 0) && ok;
        f_ = nullptr;
        return ok;
    }

    std::uint64_t bytesWritten() const { return bytes_; }

private:
    static void put16(unsigned char* p, unsigned v) { p[0] = static_cast<unsigned char>(v); p[1] = static_cast<unsigned char>(v >> 8); }
    static void put32(unsigned char* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) { p[i] = static_cast<unsigned char>(v >> (8 * i)); } }
    std::filesystem::path path_;
    Format format_;
    unsigned channels_ = 1;
    std::uint32_t rate_ = 0;
    std::FILE* f_ = nullptr;
    std::uint64_t bytes_ = 0;
    std::vector<std::int16_t> buf16_;
};

// The whole job to a file: plan, write, patch the header. On failure the partial file is left (it is a valid WAV of what was written).
inline BoxResult exportBoxToFile(TransportSource& src, const BoxSpec& spec, const std::filesystem::path& path, const std::atomic<bool>* cancel = nullptr,
                                 const std::function<void(double)>& progress = nullptr) {
    FileSink sink(path, spec.format);
    BoxResult r = exportBox(src, spec, sink, cancel, progress);
    if (!sink.close() && r.ok) { r.ok = false; r.error = "the output file could not be finished"; }
    return r;
}

}  // namespace cascade::dsp::boxexport
