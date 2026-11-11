// sigmeas_hopchirp.hpp - FREQUENCY-HOPPING and CHIRP TRACKING on a selected span. Row:
// "frequency-hopping and chirp tracking" (REMAINING-GAPS.md, Signal analysis and identification).
//
// ONE TRACK, TWO READINGS. `trackPeak` cuts the span into Hann-windowed frames (default 2^k samples
// of about 5 ms, hop of a quarter frame), takes the strongest bin of each frame and refines it by a
// parabola through the log power of its three bins; a frame whose peak is less than `minSnrDb` over
// the median bin is marked not-ok (nothing to track in it). The result is a list of
// (time, frequency, level, ok) at one point per hop.
//
//  - `analyseHops` reads the track as a frequency-hopper: a DWELL is a run of ok frames whose peak
//    stays within `tolHz` of the dwell's running mean (default 2.5 bins); the hop rate is the
//    reciprocal of the dwell pitch, which is the slope of the dwell midpoints against their whole-hop
//    index (the first and last dwell are cut by the span and are left out), so the quantisation of
//    the frame hop averages out; `distinctFreqs` clusters the dwell frequencies (the hop set) and
//    `channelSpacingHz` is the median step between neighbouring members of it.
//  - `analyseChirps` reads it as a sweeper: a SWEEP is a run of at least `minFrames` ok frames that a
//    straight line follows to within `fitTolHz` (default 1.5 bins) and that covers at least
//    `minSweepBins` bins; its slope is the least-squares slope of frequency against time (Hz per
//    second, signed), and the report gives the median slope, the median sweep width, the direction
//    (+1 up, -1 down, 0 mixed) and the repeat period (the pitch of the start times of the same-direction
//    sweeps, fitted as for the hop dwells; needs at least 3). A sawtooth chirp (LoRa-like) and a triangle both read correctly; a
//    chirp so fast that it crosses more than a few bins per FRAME smears and is not tracked.
//
// The window gives a bin spacing of rate / frameSamples; a hop or a slope is resolved only to the
// frame hop (frameSamples / 4 / rate seconds).
//
// ACCURACY, pinned in tests/test_sigmeas_hopchirp.cpp: hop rate within 2 %, dwell frequencies within
// one bin, chirp slope within 2 % and sweep period within 1 %, on generated signals with noise, a
// frequency offset and a 300 ppm clock error.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_HOPCHIRP_HPP
#define CASCADE_DSP_SIGMEAS_HOPCHIRP_HPP

#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct TrackParams {
    std::size_t frameSamples = 0;   // power of two; 0 = about 5 ms (64 .. 4096)
    std::size_t hopSamples = 0;     // 0 = frame / 4
    double minSnrDb = 10.0;         // peak over the median bin
};

struct FreqTrack {
    bool valid = false;
    const char* reason = why::kEmpty;
    double rate = 0.0;
    std::size_t frameSamples = 0;
    double binHz = 0.0;
    double stepS = 0.0;                 // time between points
    std::vector<double> timeS;          // frame centres
    std::vector<double> freqHz;         // from the span centre
    std::vector<double> peakDbfs;
    std::vector<char> ok;
    std::size_t okCount = 0;
};

inline FreqTrack trackPeak(const Cf* x, std::size_t n, double rate, const TrackParams& prm = {}) {
    FreqTrack t;
    if (const char* bad = checkIq(x, n, rate, 256)) { t.reason = bad; return t; }
    std::size_t N = prm.frameSamples;
    if (N == 0) {
        N = std::min<std::size_t>(4096, std::max<std::size_t>(64, floorPow2(static_cast<std::size_t>(rate / 200.0))));
    }
    if (!isPow2(N) || N < 32 || !std::isfinite(prm.minSnrDb) || prm.minSnrDb < 0.0) {
        t.reason = why::kBadParam;
        return t;
    }
    const std::size_t hop = prm.hopSamples > 0 ? prm.hopSamples : std::max<std::size_t>(1, N / 4);
    if (n < N + hop) { t.reason = why::kTooShort; return t; }
    const std::vector<double> w = makeWindow(Win::Hann, N);
    double sw2 = 0.0;
    for (double v : w) { sw2 += v * v; }
    const Fft fft(N);
    std::vector<std::complex<double>> buf(N);
    std::vector<double> pw(N), med(N);
    t.rate = rate;
    t.frameSamples = N;
    t.binHz = rate / static_cast<double>(N);
    t.stepS = static_cast<double>(hop) / rate;
    const std::size_t frames = (n - N) / hop + 1;
    t.timeS.reserve(frames);
    t.freqHz.reserve(frames);
    t.peakDbfs.reserve(frames);
    t.ok.reserve(frames);
    const double minRatio = fromDb(prm.minSnrDb);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t s0 = f * hop;
        for (std::size_t i = 0; i < N; ++i) {
            buf[i] = {static_cast<double>(x[s0 + i].real()) * w[i], static_cast<double>(x[s0 + i].imag()) * w[i]};
        }
        fft.forward(buf.data());
        // shifted order: bin k -> (k + N/2) % N so that index = frequency order
        for (std::size_t k = 0; k < N; ++k) {
            pw[(k + N / 2) % N] = std::norm(buf[k]) / (static_cast<double>(N) * sw2);
        }
        med = pw;
        const double median = percentileInPlace(med, 0.5);
        std::size_t pk = 1;
        for (std::size_t k = 1; k + 1 < N; ++k) { if (pw[k] > pw[pk]) { pk = k; } }
        double delta = 0.0;
        {
            const double a = std::log(std::max(pw[pk - 1], 1e-300));
            const double b = std::log(std::max(pw[pk], 1e-300));
            const double c = std::log(std::max(pw[pk + 1], 1e-300));
            const double den = a - 2.0 * b + c;
            if (den != 0.0) { delta = std::min(0.5, std::max(-0.5, 0.5 * (a - c) / den)); }
        }
        const bool good = pw[pk] > 0.0 && pw[pk] >= median * minRatio;
        t.timeS.push_back((static_cast<double>(s0) + 0.5 * static_cast<double>(N)) / rate);
        t.freqHz.push_back((static_cast<double>(pk) + delta - static_cast<double>(N) / 2.0) * t.binHz);
        t.peakDbfs.push_back(toDb(pw[pk]));
        t.ok.push_back(good ? 1 : 0);
        if (good) { ++t.okCount; }
    }
    t.valid = true;
    t.reason = why::kOk;
    return t;
}

namespace detail {

// Pitch of a roughly periodic series of times: the median spacing sets the whole-period index of
// every entry, then a least-squares line through (index, time) gives the pitch, so the frame
// quantisation of each entry averages out instead of setting the answer. NaN below two entries.
inline double pitchOfTimes(const std::vector<double>& times) {
    if (times.size() < 2) { return kNaN; }
    std::vector<double> sp;
    for (std::size_t k = 1; k < times.size(); ++k) { sp.push_back(times[k] - times[k - 1]); }
    const double med = percentileInPlace(sp, 0.5);
    if (!(med > 0.0)) { return kNaN; }
    const double N = static_cast<double>(times.size());
    double sm = 0, se = 0, smm = 0, sme = 0;
    for (std::size_t k = 0; k < times.size(); ++k) {
        const double m = std::round((times[k] - times[0]) / med);
        sm += m; se += times[k]; smm += m * m; sme += m * times[k];
    }
    const double den = N * smm - sm * sm;
    const double pitch = den > 0.0 ? (N * sme - sm * se) / den : med;
    return pitch > 0.0 ? pitch : kNaN;
}

}  // namespace detail

// ---- hopping ------------------------------------------------------------------------------------
struct Dwell {
    double startS = 0.0;     // first ok frame centre
    double endS = 0.0;       // last ok frame centre
    double freqHz = 0.0;     // median of the dwell
    std::size_t frames = 0;
};

struct HopParams {
    double tolHz = 0.0;          // 0 = 2.5 bins
    std::size_t minFrames = 2;   // a dwell is at least this many frames
};

struct HopReport {
    bool valid = false;
    const char* reason = why::kEmpty;
    std::vector<Dwell> dwells;
    std::size_t hopCount = 0;           // jumps between listed dwells
    double hopRateHz = kNaN;
    double dwellS = kNaN;               // 1 / hopRateHz
    std::vector<double> distinctFreqs;  // the hop set, ascending
    double channelSpacingHz = kNaN;
};

inline HopReport analyseHops(const FreqTrack& t, const HopParams& prm = {}) {
    HopReport r;
    if (!t.valid) { r.reason = t.reason; return r; }
    if (!(prm.tolHz >= 0.0) || !std::isfinite(prm.tolHz) || prm.minFrames == 0) {
        r.reason = why::kBadParam;
        return r;
    }
    const double tol = prm.tolHz > 0.0 ? prm.tolHz : 2.5 * t.binHz;
    const std::size_t F = t.freqHz.size();
    std::size_t i = 0;
    while (i < F) {
        if (!t.ok[i]) { ++i; continue; }
        std::size_t j = i;
        double sum = t.freqHz[i];
        std::size_t cnt = 1;
        while (j + 1 < F && t.ok[j + 1] && std::fabs(t.freqHz[j + 1] - sum / static_cast<double>(cnt)) < tol) {
            ++j;
            sum += t.freqHz[j];
            ++cnt;
        }
        if (cnt >= prm.minFrames) {
            Dwell d;
            d.startS = t.timeS[i];
            d.endS = t.timeS[j];
            d.frames = cnt;
            std::vector<double> fr(t.freqHz.begin() + static_cast<std::ptrdiff_t>(i),
                                   t.freqHz.begin() + static_cast<std::ptrdiff_t>(j) + 1);
            d.freqHz = percentileInPlace(fr, 0.5);
            r.dwells.push_back(d);
        }
        i = j + 1;
    }
    if (r.dwells.size() < 3) { r.reason = why::kNoStructure; return r; }
    // the hop set
    std::vector<double> fs;
    for (const Dwell& d : r.dwells) { fs.push_back(d.freqHz); }
    std::sort(fs.begin(), fs.end());
    {
        double sum = fs[0];
        std::size_t cnt = 1;
        for (std::size_t k = 1; k < fs.size(); ++k) {
            if (fs[k] - sum / static_cast<double>(cnt) < tol) { sum += fs[k]; ++cnt; }
            else { r.distinctFreqs.push_back(sum / static_cast<double>(cnt)); sum = fs[k]; cnt = 1; }
        }
        r.distinctFreqs.push_back(sum / static_cast<double>(cnt));
    }
    if (r.distinctFreqs.size() < 2) { r.reason = why::kNoStructure; return r; }
    if (r.distinctFreqs.size() >= 2) {
        std::vector<double> steps;
        for (std::size_t k = 1; k < r.distinctFreqs.size(); ++k) { steps.push_back(r.distinctFreqs[k] - r.distinctFreqs[k - 1]); }
        r.channelSpacingHz = percentileInPlace(steps, 0.5);
    }
    r.hopCount = r.dwells.size() - 1;
    // dwell pitch: regression of the midpoints of the complete dwells against their whole-hop index
    std::vector<double> mid;
    const std::size_t a = r.dwells.size() > 3 ? 1 : 0, b = r.dwells.size() > 3 ? r.dwells.size() - 1 : r.dwells.size();
    for (std::size_t k = a; k < b; ++k) { mid.push_back(0.5 * (r.dwells[k].startS + r.dwells[k].endS)); }
    const double pitch = detail::pitchOfTimes(mid);
    if (std::isfinite(pitch)) {
        r.dwellS = pitch;
        r.hopRateHz = 1.0 / pitch;
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

// ---- chirps ---------------------------------------------------------------------------------------
struct Sweep {
    double startS = 0.0;
    double durationS = 0.0;
    double startHz = 0.0;
    double endHz = 0.0;
    double slopeHzPerS = 0.0;
};

struct ChirpParams {
    double fitTolHz = 0.0;        // 0 = 1.5 bins
    std::size_t minFrames = 4;
    double minSweepBins = 3.0;    // a sweep covers at least this many bins
};

struct ChirpReport {
    bool valid = false;
    const char* reason = why::kEmpty;
    std::vector<Sweep> sweeps;
    double slopeHzPerS = kNaN;    // median of the sweeps, signed
    double widthHz = kNaN;        // median |end - start|
    int direction = 0;            // +1 up, -1 down, 0 mixed
    double periodS = kNaN;        // median start-to-start of same-direction sweeps; NaN below 3
};

inline ChirpReport analyseChirps(const FreqTrack& t, const ChirpParams& prm = {}) {
    ChirpReport r;
    if (!t.valid) { r.reason = t.reason; return r; }
    if (!(prm.fitTolHz >= 0.0) || !std::isfinite(prm.fitTolHz) || prm.minFrames < 3 ||
        !(prm.minSweepBins > 0.0)) {
        r.reason = why::kBadParam;
        return r;
    }
    const double tol = prm.fitTolHz > 0.0 ? prm.fitTolHz : 1.5 * t.binHz;
    const std::size_t F = t.freqHz.size();
    std::size_t i = 0;
    while (i + 2 < F) {
        if (!t.ok[i] || !t.ok[i + 1] || !t.ok[i + 2]) { ++i; continue; }
        // grow a straight run from i
        double st = 0, sf = 0, stt = 0, stf = 0;
        std::size_t cnt = 0;
        std::size_t j = i;
        double slope = 0.0, icpt = 0.0;
        auto add = [&](std::size_t k) {
            const double tt = t.timeS[k] - t.timeS[i];
            st += tt; sf += t.freqHz[k]; stt += tt * tt; stf += tt * t.freqHz[k];
            ++cnt;
        };
        auto refit = [&]() {
            const double N = static_cast<double>(cnt);
            const double den = N * stt - st * st;
            slope = den > 0.0 ? (N * stf - st * sf) / den : 0.0;
            icpt = (sf - slope * st) / N;
        };
        add(i); add(i + 1); add(i + 2);
        refit();
        j = i + 2;
        // the first three must already agree with a line
        bool good = true;
        for (std::size_t k = i; k <= i + 2; ++k) {
            if (std::fabs(t.freqHz[k] - (icpt + slope * (t.timeS[k] - t.timeS[i]))) > tol) { good = false; }
        }
        if (!good) { ++i; continue; }
        while (j + 1 < F && t.ok[j + 1]) {
            const double pred = icpt + slope * (t.timeS[j + 1] - t.timeS[i]);
            if (std::fabs(t.freqHz[j + 1] - pred) > tol) { break; }
            ++j;
            add(j);
            refit();
        }
        const double span = std::fabs(slope) * (t.timeS[j] - t.timeS[i]);
        if (cnt >= prm.minFrames && span >= prm.minSweepBins * t.binHz) {
            Sweep s;
            s.startS = t.timeS[i];
            s.durationS = t.timeS[j] - t.timeS[i];
            s.slopeHzPerS = slope;
            s.startHz = icpt;
            s.endHz = icpt + slope * s.durationS;
            r.sweeps.push_back(s);
            i = j + 1;
        } else {
            i = (cnt >= 3 && span < prm.minSweepBins * t.binHz) ? j + 1 : i + 1;
        }
    }
    if (r.sweeps.empty()) { r.reason = why::kNoStructure; return r; }
    std::vector<double> sl, wd;
    int up = 0, down = 0;
    for (const Sweep& s : r.sweeps) {
        sl.push_back(s.slopeHzPerS);
        wd.push_back(std::fabs(s.endHz - s.startHz));
        (s.slopeHzPerS > 0 ? up : down)++;
    }
    r.slopeHzPerS = percentileInPlace(sl, 0.5);
    r.widthHz = percentileInPlace(wd, 0.5);
    r.direction = (up > 0 && down > 0) ? 0 : (up > 0 ? 1 : -1);
    // repeat period over the sweeps of the majority direction
    const int dom = up >= down ? 1 : -1;
    std::vector<double> starts;
    for (const Sweep& s : r.sweeps) { if ((s.slopeHzPerS > 0 ? 1 : -1) == dom) { starts.push_back(s.startS); } }
    if (starts.size() >= 3) { r.periodS = detail::pitchOfTimes(starts); }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_HOPCHIRP_HPP
