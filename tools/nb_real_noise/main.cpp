// nb_real_noise - measures the noise blanker (src/dsp/noise_blanker.hpp) on a REAL I/Q recording of power-line
// impulse noise, through the real Vfo (the product's channel path, with the blanker hook), and times its cost.
//
// An off-line measuring tool, not a test: not registered with ctest, not in the default build (build it by
// name: cmake --build <dir> --config Release --target nb_real_noise). The recording is passed as an argument and is
// never copied anywhere by this tool.
//
//   nb_real_noise <iq.wav> spectrum
//   nb_real_noise <iq.wav> measure  --quiet HZ --sig HZ --strong HZ [--iq lr|rl] [--thr DB]
//   nb_real_noise <iq.wav> sweep    --quiet HZ --sig HZ [--iq lr|rl]
//   nb_real_noise <iq.wav> cpu      --sig HZ [--iq lr|rl]
//   nb_real_noise <iq.wav> audio    --sig HZ --out DIR --best DB [--mode usb|am] [--start S] [--dur S] [--gain G] [--iq lr|rl]
//
// --iq lr: I is the left channel, Q the right (default). rl: the other way round.
//
// Power is measured on the Vfo output (after the narrow filter) in 1 ms bins, from 1 s into the pass on (filter and
// blanker settling). Percentiles are of those bins. Blanked share and stand-down share are the blanker's own
// readouts (last 100 ms), sampled every 10 ms of signal.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/demod.hpp"
#include "dsp/fft.hpp"
#include "dsp/resampler.hpp"
#include "dsp/vfo.hpp"

using cascade::dsp::Vfo;
using cf = std::complex<float>;

namespace {

struct Wav {
    std::vector<cf> x;
    double fs = 0.0;
};

bool loadWav(const char* path, bool swapIq, Wav& w) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { return false; }
    unsigned char h[12];
    if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        return false;
    }
    int ch = 0, bits = 0;
    uint32_t rate = 0;
    for (;;) {
        unsigned char ck[8];
        if (std::fread(ck, 1, 8, f) != 8) { std::fclose(f); return false; }
        uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | (static_cast<uint32_t>(ck[7]) << 24);
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            std::vector<unsigned char> b(sz);
            if (std::fread(b.data(), 1, sz, f) != sz) { std::fclose(f); return false; }
            ch = b[2] | (b[3] << 8);
            rate = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
            bits = b[14] | (b[15] << 8);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            if (ch != 2 || bits != 16) { std::fclose(f); return false; }
            const size_t frames = sz / 4;
            std::vector<int16_t> raw(frames * 2);
            const size_t got = std::fread(raw.data(), 4, frames, f);
            raw.resize(got * 2);
            w.x.resize(got);
            for (size_t i = 0; i < got; ++i) {
                const float l = raw[2 * i] / 32768.0f, r = raw[2 * i + 1] / 32768.0f;
                w.x[i] = swapIq ? cf(r, l) : cf(l, r);
            }
            w.fs = rate;
            std::fclose(f);
            return true;
        } else {
            std::fseek(f, static_cast<long>(sz + (sz & 1)), SEEK_CUR);
        }
    }
}

double db10(double v) { return 10.0 * std::log10(v + 1e-30); }

struct Stats { double mean = 0, med = 0, p99 = 0; };  // linear power

Stats statsOf(std::vector<double> v) {
    Stats s;
    if (v.empty()) { return s; }
    double sum = 0;
    for (double d : v) { sum += d; }
    s.mean = sum / static_cast<double>(v.size());
    std::sort(v.begin(), v.end());
    s.med = v[v.size() / 2];
    s.p99 = v[std::min(v.size() - 1, static_cast<size_t>(0.99 * static_cast<double>(v.size())))];
    return s;
}

struct Run {
    Stats s;
    std::vector<double> bins;  // 1 ms bin powers, linear
    std::vector<char> stand;   // fine passes: the blanker was standing down at the end of the bin
    double blankedPct = 0;  // mean of the blanker's 100 ms readout
    double standPct = 0;    // share of readings that were standing down
    size_t lookAhead = 0;
    double channelRate = 0;
};

// One pass of [begin, end) of the file through a Vfo, in blocks of 10 ms. keepOut gets every output sample.
Run runVfo(const Wav& w, double off, double bw, bool nb, float thr, size_t begin = 0, size_t end = 0,
           std::vector<cf>* keepOut = nullptr, bool fine = false) {
    Vfo v(w.fs, 1, bw);
    v.setOffsetHz(off);
    if (nb) {
        v.setNoiseBlankerThresholdDb(thr);
        v.setNoiseBlankerEnabled(true);
    }
    Run r;
    r.channelRate = v.channelRateHz();
    r.lookAhead = v.noiseBlankerLookAheadSamples();
    if (end == 0 || end > w.x.size()) { end = w.x.size(); }
    const size_t blockN = static_cast<size_t>(w.fs / (fine ? 1000.0 : 100.0));
    const size_t binN = static_cast<size_t>(w.fs / 1000.0);
    const size_t skip = static_cast<size_t>(w.fs);  // 1 s
    std::vector<cf> out(blockN + 16);
    std::vector<double> bins;
    double acc = 0;
    size_t accN = 0, produced = 0, readings = 0, standing = 0;
    double blankedSum = 0;
    for (size_t pos = begin; pos < end; pos += blockN) {
        const size_t n = std::min(blockN, end - pos);
        const size_t m = v.process(w.x.data() + pos, n, out.data(), out.size());
        for (size_t i = 0; i < m; ++i) {
            if (keepOut) { keepOut->push_back(out[i]); }
            if (produced++ < skip) { continue; }
            acc += static_cast<double>(std::norm(out[i]));
            if (++accN == binN) {
                bins.push_back(acc / static_cast<double>(binN));
                if (fine) { r.stand.push_back(v.noiseBlankerStandingDown() ? 1 : 0); }
                acc = 0;
                accN = 0;
            }
        }
        if (produced > skip) {
            ++readings;
            blankedSum += v.noiseBlankerBlankedPercent();
            if (v.noiseBlankerStandingDown()) { ++standing; }
        }
    }
    r.bins = bins;
    r.s = statsOf(std::move(bins));
    if (readings) {
        r.blankedPct = blankedSum / static_cast<double>(readings);
        r.standPct = 100.0 * static_cast<double>(standing) / static_cast<double>(readings);
    }
    return r;
}

const char* argStr(int argc, char** argv, const char* key, const char* def) {
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) { return argv[i + 1]; }
    }
    return def;
}
double argNum(int argc, char** argv, const char* key, double def) {
    const char* s = argStr(argc, argv, key, nullptr);
    return s ? std::atof(s) : def;
}

// ---------------------------------------------------------------------------------------------------------------
// spectrum: Welch mean and median PSD, peaks, the +24 kHz test for both I/Q orientations, quiet-offset search.
struct Spec {
    std::vector<double> f, mean, med;  // f in Hz; PSD linear (arbitrary unit), fftshifted
};

Spec welch(const Wav& w, size_t N) {
    cascade::dsp::ComplexFFT fft(N);
    std::vector<float> win(N);
    for (size_t i = 0; i < N; ++i) {
        win[i] = 0.5f - 0.5f * std::cos(6.283185307f * static_cast<float>(i) / static_cast<float>(N));
    }
    const size_t hop = N / 2;
    const size_t frames = (w.x.size() - N) / hop + 1;
    std::vector<cf> in(N), out(N);
    std::vector<float> all(frames * N);
    Spec s;
    s.mean.assign(N, 0.0);
    for (size_t k = 0; k < frames; ++k) {
        for (size_t i = 0; i < N; ++i) { in[i] = w.x[k * hop + i] * win[i]; }
        fft.forward(in.data(), out.data());
        for (size_t b = 0; b < N; ++b) {
            const size_t sb = (b + N / 2) % N;  // fftshift
            const float p = std::norm(out[sb]);
            all[k * N + b] = p;
            s.mean[b] += p;
        }
    }
    s.med.assign(N, 0.0);
    std::vector<float> col(frames);
    for (size_t b = 0; b < N; ++b) {
        for (size_t k = 0; k < frames; ++k) { col[k] = all[k * N + b]; }
        std::nth_element(col.begin(), col.begin() + static_cast<long>(frames / 2), col.end());
        s.med[b] = col[frames / 2];
        s.mean[b] /= static_cast<double>(frames);
    }
    s.f.resize(N);
    for (size_t b = 0; b < N; ++b) {
        s.f[b] = (static_cast<double>(b) - static_cast<double>(N / 2)) * w.fs / static_cast<double>(N);
    }
    return s;
}

double bandMax(const Spec& s, const std::vector<double>& p, double lo, double hi, double* at = nullptr) {
    double m = 0;
    for (size_t i = 0; i < s.f.size(); ++i) {
        if (s.f[i] >= lo && s.f[i] <= hi && p[i] > m) {
            m = p[i];
            if (at) { *at = s.f[i]; }
        }
    }
    return m;
}
double bandMean(const Spec& s, const std::vector<double>& p, double lo, double hi) {
    double a = 0;
    size_t n = 0;
    for (size_t i = 0; i < s.f.size(); ++i) {
        if (s.f[i] >= lo && s.f[i] <= hi) { a += p[i]; ++n; }
    }
    return n ? a / static_cast<double>(n) : 0.0;
}

// Prominent narrow peaks of the MEAN spectrum: height over the median of the +-1 kHz around (dB).
void printPeaks(const Spec& s, double fs, size_t N, size_t maxN) {
    struct P { double f, db, prom; };
    std::vector<P> ps;
    const size_t hw = static_cast<size_t>(1000.0 / (fs / static_cast<double>(N)));
    for (size_t k = hw + 1; k + hw + 1 < N; ++k) {
        if (s.mean[k] < s.mean[k - 1] || s.mean[k] < s.mean[k + 1]) { continue; }
        std::vector<double> nb(s.mean.begin() + static_cast<long>(k - hw), s.mean.begin() + static_cast<long>(k + hw));
        std::nth_element(nb.begin(), nb.begin() + static_cast<long>(nb.size() / 2), nb.end());
        const double prom = db10(s.mean[k]) - db10(nb[nb.size() / 2]);
        if (prom > 6.0) { ps.push_back({s.f[k], db10(s.mean[k]), prom}); }
    }
    std::sort(ps.begin(), ps.end(), [](const P& a, const P& b) { return a.db > b.db; });
    std::vector<P> sel;
    for (const P& p : ps) {
        bool ok = true;
        for (const P& q : sel) {
            if (std::fabs(p.f - q.f) < 300.0) { ok = false; break; }
        }
        if (ok) { sel.push_back(p); }
        if (sel.size() == maxN) { break; }
    }
    std::printf(" narrow peaks of the MEAN spectrum (Hz, dB, prominence dB), strongest first:");
    for (const P& p : sel) { std::printf(" [%+.0f %.1f %.1f]", p.f, p.db, p.prom); }
    std::printf("\n");
}

// The long-term spectrum is dominated by the impulses (every 340 ms frame has several). The QUIET-FRAME spectrum is the
// 25th percentile over short frames (N = 512, 5.3 ms, 188 Hz bins) of each bin's power: frames without an impulse set it.
void printQuietSpectrum(const Wav& w, bool lr) {
    const size_t N = 512;
    cascade::dsp::ComplexFFT fft(N);
    std::vector<float> win(N);
    for (size_t i = 0; i < N; ++i) { win[i] = 0.5f - 0.5f * std::cos(6.283185307f * static_cast<float>(i) / static_cast<float>(N)); }
    const size_t frames = w.x.size() / N;
    std::vector<cf> in(N), out(N);
    std::vector<float> all(frames * N);
    for (size_t k = 0; k < frames; ++k) {
        for (size_t i = 0; i < N; ++i) { in[i] = w.x[k * N + i] * win[i]; }
        fft.forward(in.data(), out.data());
        for (size_t b = 0; b < N; ++b) { all[k * N + b] = std::norm(out[(b + N / 2) % N]); }
    }
    std::vector<double> q(N);
    std::vector<float> col(frames);
    for (size_t b = 0; b < N; ++b) {
        for (size_t k = 0; k < frames; ++k) { col[k] = all[k * N + b]; }
        std::nth_element(col.begin(), col.begin() + static_cast<long>(frames / 4), col.end());
        q[b] = col[frames / 4];
    }
    auto fq = [&](size_t b) { return (static_cast<double>(b) - static_cast<double>(N / 2)) * w.fs / static_cast<double>(N); };
    std::printf(" QUIET-FRAME spectrum (25th percentile over 5.3 ms frames, dB), orientation %s; 4 kHz cells, max bin:\n", lr ? "lr" : "rl");
    for (double c = -44000; c <= 44000; c += 4000) {
        double m = 0, at = 0;
        for (size_t b = 0; b < N; ++b) {
            if (fq(b) >= c - 2000 && fq(b) < c + 2000 && q[b] > m) { m = q[b]; at = fq(b); }
        }
        std::printf("  %+6.0f: %6.1f (at %+.0f)%s", c, db10(m), at, ((static_cast<int>((c + 44000) / 4000) % 3) == 2) ? "\n" : "  |");
    }
    std::printf("\n");
    std::vector<double> s2 = q;
    std::sort(s2.begin(), s2.end());
    std::printf("  median bin %.1f dB; strongest bins:", db10(s2[N / 2]));
    std::vector<size_t> idx(N);
    for (size_t i = 0; i < N; ++i) { idx[i] = i; }
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return q[a] > q[b]; });
    std::vector<size_t> sel;
    for (size_t k : idx) {
        bool ok = true;
        for (size_t j : sel) {
            if (std::fabs(fq(k) - fq(j)) < 600.0) { ok = false; break; }
        }
        if (ok) { sel.push_back(k); }
        if (sel.size() == 8) { break; }
    }
    for (size_t k : sel) { std::printf(" [%+.0f %.1f]", fq(k), db10(q[k])); }
    std::printf("\n");
}

int cmdSpectrum(const char* path) {
    for (int orient = 0; orient < 2; ++orient) {
        Wav w;
        if (!loadWav(path, orient == 1, w)) { std::fprintf(stderr, "cannot read %s\n", path); return 2; }
        const size_t N = 32768;
        Spec s = welch(w, N);
        std::printf("== orientation %s (I=%s, Q=%s), fs %.0f Hz, %zu samples (%.1f s), bin %.2f Hz\n",
                    orient ? "rl" : "lr", orient ? "right" : "left", orient ? "left" : "right", w.fs, w.x.size(),
                    static_cast<double>(w.x.size()) / w.fs, w.fs / static_cast<double>(N));
        std::vector<size_t> idx(N);
        for (size_t i = 0; i < N; ++i) { idx[i] = i; }
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return s.med[a] > s.med[b]; });
        std::vector<size_t> pk;
        for (size_t k : idx) {
            bool ok = true;
            for (size_t j : pk) {
                if (std::fabs(s.f[k] - s.f[j]) < 1500.0) { ok = false; break; }
            }
            if (ok) { pk.push_back(k); }
            if (pk.size() == 12) { break; }
        }
        std::printf(" strongest peaks of the MEDIAN spectrum (Hz, dB re 1 FFT unit):");
        for (size_t k : pk) { std::printf(" [%+.0f %.1f]", s.f[k], db10(s.med[k])); }
        std::printf("\n");
        printPeaks(s, w.fs, N, 16);
        printQuietSpectrum(w, orient == 0);
        if (orient == 0) {
            const size_t seg = static_cast<size_t>(w.fs * 10.0);
            for (size_t b = 0; b + seg <= w.x.size(); b += seg) {
                Wav sw;
                sw.fs = w.fs;
                sw.x.assign(w.x.begin() + static_cast<long>(b), w.x.begin() + static_cast<long>(b + seg));
                std::printf(" t=%3.0f..%3.0f s:", static_cast<double>(b) / w.fs, static_cast<double>(b + seg) / w.fs);
                printPeaks(welch(sw, 16384), w.fs, 16384, 5);
            }
        }
        double at = 0;
        const double mp = bandMax(s, s.med, 22000, 26000, &at);
        const double mm = bandMax(s, s.med, -26000, -22000);
        std::printf(" max of median PSD in +22..+26 kHz: %.1f dB at %+.1f Hz; in -26..-22 kHz: %.1f dB; difference %.1f dB\n",
                    db10(mp), at, db10(mm), db10(mp) - db10(mm));
        if (orient == 0) {
            std::printf(" profile, 2 kHz cells (centre Hz: median-PSD mean dB / mean-PSD mean dB / max of mean-PSD dB):\n");
            for (double c = -47000; c <= 47000; c += 2000) {
                std::printf("  %+6.0f: %6.1f %6.1f %6.1f%s", c, db10(bandMean(s, s.med, c - 1000, c + 1000)),
                            db10(bandMean(s, s.mean, c - 1000, c + 1000)), db10(bandMax(s, s.mean, c - 1000, c + 1000)),
                            (static_cast<int>((c + 47000) / 2000) % 3 == 2) ? "\n" : "   |");
            }
            std::printf("\n");
            // The median spectrum shows stations (always there) and not impulses (rare). The narrow window (3 kHz
            // plus 500 Hz each side) must sit at the floor; the +-12 kHz around it is what the blanker's own
            // channel filter passes, so the quietest wide window breaks the tie.
            std::vector<double> srt = s.med;
            std::sort(srt.begin(), srt.end());
            std::printf(" floor (10th percentile of median PSD) %.1f dB\n", db10(srt[srt.size() / 10]));
            struct C { double off, narrow, wide; };
            std::vector<C> cs;
            for (double off = -40000; off <= 40000; off += 250) {
                cs.push_back({off, bandMax(s, s.med, off - 2000, off + 2000), bandMean(s, s.med, off - 12000, off + 12000)});
            }
            std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.narrow + 0.3 * a.wide < b.narrow + 0.3 * b.wide; });
            std::printf(" quiet candidates (offset Hz, narrow-window max dB, +-12 kHz mean dB):\n");
            for (size_t i = 0; i < 8; ++i) { std::printf("   %+.0f  %.1f  %.1f\n", cs[i].off, db10(cs[i].narrow), db10(cs[i].wide)); }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// In-channel view of one station: Welch frames of 4096 channel samples (43 ms, 23 Hz bins) from 1 s into the pass.
// Per frame: the peak bin within +-450 Hz of the channel centre (the line may drift ~150 Hz) and the mean power of the
// bins 600..1400 Hz away on either side (the noise next to it). Medians over the frames, so impulse-struck frames do
// not decide the figure. snr = peak - floor, per frame, in dB.
struct Line { double peakDb = 0, floorDb = 0, snrDb = 0; };

Line lineStats(const std::vector<cf>& ch, double fs) {
    const size_t N = 4096;
    cascade::dsp::ComplexFFT fft(N);
    std::vector<float> win(N);
    for (size_t i = 0; i < N; ++i) { win[i] = 0.5f - 0.5f * std::cos(6.283185307f * static_cast<float>(i) / static_cast<float>(N)); }
    std::vector<cf> in(N), out(N);
    std::vector<double> pk, fl, sn;
    for (size_t b = static_cast<size_t>(fs); b + N <= ch.size(); b += N) {
        for (size_t i = 0; i < N; ++i) { in[i] = ch[b + i] * win[i]; }
        fft.forward(in.data(), out.data());
        double peak = 0, floorSum = 0;
        size_t floorN = 0;
        for (size_t k = 0; k < N; ++k) {
            const double f = (k < N / 2 ? static_cast<double>(k) : static_cast<double>(k) - static_cast<double>(N)) * fs / static_cast<double>(N);
            const double p = static_cast<double>(std::norm(out[k]));
            if (std::fabs(f) <= 450.0) {
                peak = std::max(peak, p);
            } else if (std::fabs(f) >= 600.0 && std::fabs(f) <= 1400.0) {
                floorSum += p;
                ++floorN;
            }
        }
        const double floorP = floorSum / static_cast<double>(floorN);
        pk.push_back(db10(peak));
        fl.push_back(db10(floorP));
        sn.push_back(db10(peak) - db10(floorP));
    }
    auto med = [](std::vector<double>& v) { std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end()); return v[v.size() / 2]; };
    Line l;
    l.peakDb = med(pk);
    l.floorDb = med(fl);
    l.snrDb = med(sn);
    return l;
}

// Pass at (off, bw) with the blanker off/on and the line statistics of its output.
Line lineRun(const Wav& w, double off, double bw, bool nb, float thr) {
    std::vector<cf> ch;
    runVfo(w, off, bw, nb, thr, 0, 0, &ch);
    return lineStats(ch, w.fs);
}

void printStats(const char* tag, const Stats& s) {
    std::printf("  %-10s mean %8.2f dB  median %8.2f dB  p99 %8.2f dB\n", tag, db10(s.mean), db10(s.med), db10(s.p99));
}

int cmdMeasure(const Wav& w, double quiet, double sig, double strong, float thr) {
    std::printf("channel rate chosen by the Vfo: %.0f Hz (input %.0f Hz, decimation 1); blanker look-ahead %zu samples\n",
                w.fs, w.fs, Vfo(w.fs, 1, 3000).noiseBlankerLookAheadSamples());
    std::printf("threshold %.0f dB; power on 1 ms bins of the Vfo output, dB re full scale^2\n", thr);
    Run q3[2], q5[2], sg[2];
    for (int nb = 0; nb < 2; ++nb) {
        q3[nb] = runVfo(w, quiet, 3000, nb != 0, thr);
        q5[nb] = runVfo(w, quiet, 500, nb != 0, thr);
        sg[nb] = runVfo(w, sig, 3000, nb != 0, thr);
    }
    auto tab = [&](const char* name, Run* r) {
        std::printf("%s\n", name);
        printStats("OFF", r[0].s);
        printStats("ON", r[1].s);
        std::printf("  drop       mean %8.2f dB  median %8.2f dB  p99 %8.2f dB   blanked %.2f %%  stood down %.1f %%\n",
                    db10(r[0].s.mean) - db10(r[1].s.mean), db10(r[0].s.med) - db10(r[1].s.med),
                    db10(r[0].s.p99) - db10(r[1].s.p99), r[1].blankedPct, r[1].standPct);
    };
    std::printf("\n[1] QUIET channel at %+.0f Hz\n", quiet);
    tab(" 3 kHz bandwidth", q3);
    tab(" 500 Hz bandwidth", q5);
    std::printf("\n[2] SIGNAL channel at %+.0f Hz, 3 kHz\n", sig);
    tab(" signal", sg);
    const double offR = db10(sg[0].s.mean) - db10(q3[0].s.mean), onR = db10(sg[1].s.mean) - db10(q3[1].s.mean);
    std::printf("  (signal mean)/(quiet 3 kHz mean): OFF %.2f dB, ON %.2f dB, gain in signal-to-noise %.2f dB\n", offR, onR, onR - offR);
    std::printf("  signal level change ON vs OFF (mean): %.2f dB\n", db10(sg[1].s.mean) - db10(sg[0].s.mean));
    {
        const Line a = lineRun(w, sig, 3000, false, thr), b = lineRun(w, sig, 3000, true, thr);
        std::printf("  line view (medians over 43 ms frames): line peak OFF %.2f ON %.2f dB (change %.2f dB); floor beside it OFF %.2f ON %.2f dB (change %.2f dB); line-to-floor OFF %.2f ON %.2f dB (gain %.2f dB)\n",
                    a.peakDb, b.peakDb, b.peakDb - a.peakDb, a.floorDb, b.floorDb, b.floorDb - a.floorDb, a.snrDb, b.snrDb, b.snrDb - a.snrDb);
    }
    Run st[2];
    for (int nb = 0; nb < 2; ++nb) { st[nb] = runVfo(w, strong, 3000, nb != 0, thr); }
    {  // False blanking: the bins in which the QUIET channel (blanker off) shows no impulse (under 4 x its median).
        const double lim = 4.0 * q3[0].s.med;
        double sOff = 0, sOn = 0;
        size_t n = 0, hurt = 0;
        const size_t m = std::min(sg[0].bins.size(), q3[0].bins.size());
        for (size_t i = 0; i < m; ++i) {
            if (q3[0].bins[i] > lim) { continue; }
            sOff += sg[0].bins[i];
            sOn += sg[1].bins[i];
            ++n;
            if (sg[1].bins[i] < 0.5 * sg[0].bins[i]) { ++hurt; }
        }
        std::printf("  impulse-free bins (quiet channel under 6 dB over its own median): %.1f %% of all; on those bins the station level ON vs OFF: %.3f dB; bins that lost more than 3 dB: %.2f %%\n",
                    100.0 * static_cast<double>(n) / static_cast<double>(m), db10(sOn / static_cast<double>(n)) - db10(sOff / static_cast<double>(n)),
                    100.0 * static_cast<double>(hurt) / static_cast<double>(n));
    }
    {  // Where do the strongest bins survive? A pass with 1 ms blocks gives the stand-down flag at every bin.
        const Run f0 = runVfo(w, quiet, 3000, false, thr, 0, 0, nullptr, true);
        const Run f1 = runVfo(w, quiet, 3000, true, thr, 0, 0, nullptr, true);
        const size_t m = std::min(f0.bins.size(), f1.bins.size());
        std::vector<size_t> idx0(m), idx1(m);
        for (size_t i = 0; i < m; ++i) { idx0[i] = idx1[i] = i; }
        std::sort(idx0.begin(), idx0.end(), [&](size_t a, size_t b) { return f0.bins[a] > f0.bins[b]; });
        std::sort(idx1.begin(), idx1.end(), [&](size_t a, size_t b) { return f1.bins[a] > f1.bins[b]; });
        const size_t top = m / 100;
        size_t sd = 0, sd1 = 0, all = 0, kept = 0;
        for (size_t i = 0; i < top; ++i) {
            if (f1.stand[idx0[i]]) { ++sd; }
            if (f1.stand[idx1[i]]) { ++sd1; }
            if (f1.bins[idx0[i]] > 0.5 * f0.bins[idx0[i]]) { ++kept; }
        }
        for (size_t i = 0; i < m; ++i) { all += f1.stand[i] ? 1 : 0; }
        std::printf("\n[diag] quiet 3 kHz, threshold %.0f dB, 1 ms blocks: bins standing down %.1f %%; of the strongest 1%% of OFF bins: %.1f %% fell in stand-down, %.1f %% kept more than half their power; of the strongest 1%% of ON bins: %.1f %% fell in stand-down\n",
                    thr, 100.0 * static_cast<double>(all) / static_cast<double>(m), 100.0 * static_cast<double>(sd) / static_cast<double>(top),
                    100.0 * static_cast<double>(kept) / static_cast<double>(top), 100.0 * static_cast<double>(sd1) / static_cast<double>(top));
    }
    std::printf("\n[5] STRONGEST station at %+.0f Hz, 3 kHz, threshold %.0f dB\n", strong, thr);
    printStats("OFF", st[0].s);
    printStats("ON", st[1].s);
    std::printf("  level change ON vs OFF: mean %.3f dB, median %.3f dB; blanked %.3f %%; stood down %.1f %%\n",
                db10(st[1].s.mean) - db10(st[0].s.mean), db10(st[1].s.med) - db10(st[0].s.med), st[1].blankedPct, st[1].standPct);
    {
        const Line a = lineRun(w, strong, 3000, false, thr), b = lineRun(w, strong, 3000, true, thr);
        std::printf("  line view (medians over 43 ms frames): line peak OFF %.2f ON %.2f dB (change %.2f dB); floor beside it OFF %.2f ON %.2f dB (change %.2f dB); line-to-floor OFF %.2f ON %.2f dB (gain %.2f dB)\n",
                    a.peakDb, b.peakDb, b.peakDb - a.peakDb, a.floorDb, b.floorDb, b.floorDb - a.floorDb, a.snrDb, b.snrDb, b.snrDb - a.snrDb);
    }
    {  // False blanking: the bins in which the QUIET channel (blanker off) shows no impulse (under 4 x its median).
        const double lim = 4.0 * q3[0].s.med;
        double sOff = 0, sOn = 0;
        size_t n = 0, hurt = 0;
        const size_t m = std::min(st[0].bins.size(), q3[0].bins.size());
        for (size_t i = 0; i < m; ++i) {
            if (q3[0].bins[i] > lim) { continue; }
            sOff += st[0].bins[i];
            sOn += st[1].bins[i];
            ++n;
            if (st[1].bins[i] < 0.5 * st[0].bins[i]) { ++hurt; }
        }
        std::printf("  impulse-free bins (quiet channel under 6 dB over its own median): %.1f %% of all; on those bins the station level ON vs OFF: %.3f dB; bins that lost more than 3 dB: %.2f %%\n",
                    100.0 * static_cast<double>(n) / static_cast<double>(m), db10(sOn / static_cast<double>(n)) - db10(sOff / static_cast<double>(n)),
                    100.0 * static_cast<double>(hurt) / static_cast<double>(n));
    }
    return 0;
}

int cmdSweep(const Wav& w, double quiet, double sig) {
    const float thrs[] = {6, 9, 12, 15, 18, 21, 24, 27, 30};
    const Run q3o = runVfo(w, quiet, 3000, false, 15), q5o = runVfo(w, quiet, 500, false, 15), sgo = runVfo(w, sig, 3000, false, 15);
    const Line lo = lineRun(w, sig, 3000, false, 15);
    std::printf("OFF: quiet3k mean %.2f median %.2f p99 %.2f dB | quiet500 mean %.2f p99 %.2f dB | signal mean %.2f dB | line peak %.2f floor %.2f snr %.2f dB\n",
                db10(q3o.s.mean), db10(q3o.s.med), db10(q3o.s.p99), db10(q5o.s.mean), db10(q5o.s.p99), db10(sgo.s.mean), lo.peakDb, lo.floorDb, lo.snrDb);
    std::printf("thr | quiet3k: blank%% stand%% dMean dMed dP99 | quiet500: dMean dP99 | signal: blank%% stand%% dMean | ratio gain | line: dPeak dFloor SNR gain\n");
    for (float t : thrs) {
        const Run a = runVfo(w, quiet, 3000, true, t), b = runVfo(w, quiet, 500, true, t), c = runVfo(w, sig, 3000, true, t);
        const Line ln = lineRun(w, sig, 3000, true, t);
        const double offR = db10(sgo.s.mean) - db10(q3o.s.mean), onR = db10(c.s.mean) - db10(a.s.mean);
        std::printf("%3.0f | %6.2f %6.1f %6.2f %6.2f %6.2f | %6.2f %6.2f | %6.2f %6.1f %6.2f | %6.2f | %6.2f %6.2f %6.2f\n", t, a.blankedPct, a.standPct,
                    db10(q3o.s.mean) - db10(a.s.mean), db10(q3o.s.med) - db10(a.s.med), db10(q3o.s.p99) - db10(a.s.p99),
                    db10(q5o.s.mean) - db10(b.s.mean), db10(q5o.s.p99) - db10(b.s.p99), c.blankedPct, c.standPct,
                    db10(sgo.s.mean) - db10(c.s.mean), onR - offR, ln.peakDb - lo.peakDb, ln.floorDb - lo.floorDb, ln.snrDb - lo.snrDb);
        std::fflush(stdout);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// One timed pass of the whole input through a fresh Vfo.
double timeVfoOnce(const std::vector<cf>& in, double fs, unsigned decim, double bw, double off, bool nb, std::vector<cf>& out) {
    Vfo v(fs, decim, bw);
    v.setOffsetHz(off);
    if (nb) { v.setNoiseBlankerEnabled(true); }
    const auto t0 = std::chrono::steady_clock::now();
    const size_t blockN = 16384;
    size_t sink = 0;
    for (size_t pos = 0; pos < in.size(); pos += blockN) {
        sink += v.process(in.data() + pos, std::min(blockN, in.size() - pos), out.data(), out.size());
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (sink == 0) { std::printf("?\n"); }
    return std::chrono::duration<double>(t1 - t0).count();
}

// Best of five, OFF and ON interleaved (so both meet the same background load); returns {off, on} seconds.
std::pair<double, double> bestOfFivePair(const std::vector<cf>& in, double fs, unsigned decim, double bw, double off) {
    std::vector<cf> out(in.size() / decim + 64);
    double bo = 1e30, bn = 1e30;
    for (int r = 0; r < 5; ++r) {
        bo = std::min(bo, timeVfoOnce(in, fs, decim, bw, off, false, out));
        bn = std::min(bn, timeVfoOnce(in, fs, decim, bw, off, true, out));
    }
    return {bo, bn};
}

int cmdCpu(const Wav& w, double sig) {
    const double n = static_cast<double>(w.x.size());
    const auto pa = bestOfFivePair(w.x, w.fs, 1, 3000, sig);
    const double off = pa.first, on = pa.second;
    std::printf("A. the recording through Vfo(%.0f, 1, 3000), offset %+.0f Hz (channel rate %.0f Hz = input rate), best of 5:\n", w.fs, sig, w.fs);
    std::printf("   OFF %.4f s = %.2f ns/input sample; ON %.4f s = %.2f ns/input sample; blanker adds %.2f ns per sample (%.1f %%)\n",
                off, off / n * 1e9, on, on / n * 1e9, (on - off) / n * 1e9, 100.0 * (on - off) / off);
    std::printf("   blanker alone as a share of one core if its stream were 2.4 MS/s: %.2f %%\n", (on - off) / n * 2.4e6 * 100.0);
    // B. the product's real geometry: 2.4 MS/s input, decimation 12, channel 200 kHz, synthetic noise + clicks.
    const double fs = 2.4e6;
    const size_t N = static_cast<size_t>(fs * 5.0);
    std::vector<cf> x(N);
    uint32_t s = 12345;
    auto rnd = [&]() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(s >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
    };
    for (size_t i = 0; i < N; ++i) { x[i] = cf(3e-4f * rnd(), 3e-4f * rnd()); }
    for (size_t i = 1000; i + 8 < N; i += 24000) {  // 100 clicks a second
        for (size_t k = 0; k < 6; ++k) { x[i + k] += cf(0.2f, 0.1f); }
    }
    const auto pb = bestOfFivePair(x, fs, 12, 3000, 24000.0);
    const double off2 = pb.first, on2 = pb.second;
    const double nn = static_cast<double>(N);
    std::printf("B. synthetic 2.4 MS/s noise + 100 clicks/s through Vfo(2400000, 12, 3000) (channel rate 200000 Hz), best of 5:\n");
    std::printf("   OFF %.4f s = %.2f ns/input sample; ON %.4f s = %.2f ns/input sample; blanker adds %.3f ns/input sample\n",
                off2, off2 / nn * 1e9, on2, on2 / nn * 1e9, (on2 - off2) / nn * 1e9);
    std::printf("   share of one core at 2.4 MS/s: Vfo OFF %.2f %%, Vfo ON %.2f %%, blanker alone %.3f %%\n",
                off2 / nn * 2.4e6 * 100.0, on2 / nn * 2.4e6 * 100.0, (on2 - off2) / nn * 2.4e6 * 100.0);
    // C. the blanker alone, called directly on a stream (no Vfo): the per-sample cost, best of 5.
    auto timeNb = [](const std::vector<cf>& in, double rate, float thr) {
        double best = 1e30;
        std::vector<cf> out(in.size());
        for (int r = 0; r < 5; ++r) {
            cascade::dsp::NoiseBlanker nb(rate);
            nb.setThresholdDb(thr);
            nb.setEnabled(true);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t pos = 0; pos < in.size(); pos += 4096) { nb.process(in.data() + pos, out.data() + pos, std::min<size_t>(4096, in.size() - pos)); }
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
            if (out[in.size() / 2] == cf(12345.f, 0.f)) { std::printf("?\n"); }
        }
        return best / static_cast<double>(in.size()) * 1e9;
    };
    std::vector<cf> ch(x.begin(), x.begin() + 2000000);  // 2.4 MS/s synthetic stream used AS a 200 kHz channel stream
    const double nsReal = timeNb(w.x, w.fs, 15), nsSyn = timeNb(ch, 200000.0, 15);
    std::printf("C. NoiseBlanker::process alone, best of 5: the recording as a 96 kHz stream (many blanked/standing samples): %.2f ns/sample; synthetic 200 kHz stream with 100 clicks/s: %.2f ns/sample\n", nsReal, nsSyn);
    std::printf("   as a share of one core: 200 kHz channel (2.4 MS/s input, decimation 12): %.3f %% (real-data figure) / %.3f %% (synthetic); even at a 2.4 MHz channel (decimation 1) it would be %.2f %%\n",
                nsReal * 200000.0 * 1e-7, nsSyn * 200000.0 * 1e-7, nsReal * 2.4e6 * 1e-7);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
void writeWav48(const std::string& path, const std::vector<float>& a, float gain) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    const uint32_t n = static_cast<uint32_t>(a.size()), data = n * 2, rate = 48000;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(1); w32(rate); w32(rate * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, f);
    w32(data);
    for (float v : a) {
        const float c = std::max(-1.0f, std::min(1.0f, v * gain));
        const int16_t s = static_cast<int16_t>(std::lrintf(c * 32767.0f));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
}

std::vector<float> demodAudio(const Wav& w, double sig, bool am, bool nb, float thr, double start, double dur) {
    const double bw = am ? 10000.0 : 12000.0;  // the pipeline gives a sideband mode 4 x the 3 kHz width
    const double lead = std::min(start, 2.0);
    const size_t b = static_cast<size_t>((start - lead) * w.fs);
    const size_t e = std::min(w.x.size(), static_cast<size_t>((start + dur) * w.fs));
    std::vector<cf> ch;
    runVfo(w, sig, bw, nb, thr, b, e, &ch);
    cascade::dsp::Demodulator d(w.fs);
    d.setMode(am ? cascade::dsp::DemodMode::AM : cascade::dsp::DemodMode::USB);
    std::vector<float> a(ch.size());
    d.process(ch.data(), ch.size(), a.data());
    cascade::dsp::RationalResampler rs(1, static_cast<unsigned>(w.fs / 48000.0));
    std::vector<float> o(rs.maxOut(a.size()));
    o.resize(rs.process(a.data(), a.size(), o.data(), o.size()));
    const size_t skip = static_cast<size_t>(lead * 48000.0);
    return std::vector<float>(o.begin() + static_cast<long>(std::min(skip, o.size())), o.end());
}

int cmdAudio(const Wav& w, double sig, const std::string& dir, float best, bool am, double start, double dur, float gainArg) {
    struct F { std::string name; std::vector<float> a; };
    std::vector<F> files;
    files.push_back({"off", demodAudio(w, sig, am, false, 15, start, dur)});
    files.push_back({"on15", demodAudio(w, sig, am, true, 15, start, dur)});
    char nm[32];
    std::snprintf(nm, sizeof nm, "on%.0f_best", best);
    files.push_back({nm, demodAudio(w, sig, am, true, best, start, dur)});
    float peak = 0;
    for (auto& f : files) {
        float p = 0, rms = 0;
        for (float v : f.a) { p = std::max(p, std::fabs(v)); rms += v * v; }
        std::printf("  %-12s peak %.5f  rms %.5f (%zu samples)\n", f.name.c_str(), p, std::sqrt(rms / static_cast<float>(f.a.size())), f.a.size());
        peak = std::max(peak, p);
    }
    const float gain = gainArg > 0 ? gainArg : (peak > 0 ? 0.9f / peak : 1.0f);  // --gain: one gain across several runs
    std::printf("common gain %.3f (0.9 over the loudest peak of this run, unless --gain was given)\n", gain);
    for (auto& f : files) {
        const std::string p = dir + "/" + (am ? "am_" : "usb_") + f.name + ".wav";
        writeWav48(p, f.a, gain);
        std::printf("wrote %s\n", p.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: nb_real_noise <iq.wav> spectrum|measure|sweep|cpu|audio [options] (see the top of main.cpp)\n");
        return 2;
    }
    const std::string cmd = argv[2];
    if (cmd == "spectrum") { return cmdSpectrum(argv[1]); }
    Wav w;
    const bool swapIq = std::strcmp(argStr(argc, argv, "--iq", "lr"), "rl") == 0;
    if (!loadWav(argv[1], swapIq, w)) { std::fprintf(stderr, "cannot read %s as a 16-bit stereo WAV\n", argv[1]); return 2; }
    std::printf("%s: %zu samples at %.0f Hz, I=%s\n", argv[1], w.x.size(), w.fs, swapIq ? "right" : "left");
    const double sig = argNum(argc, argv, "--sig", 24000.0), quiet = argNum(argc, argv, "--quiet", 0.0);
    if (cmd == "measure") {
        return cmdMeasure(w, quiet, sig, argNum(argc, argv, "--strong", sig), static_cast<float>(argNum(argc, argv, "--thr", 15.0)));
    }
    if (cmd == "sweep") { return cmdSweep(w, quiet, sig); }
    if (cmd == "cpu") { return cmdCpu(w, sig); }
    if (cmd == "audio") {
        return cmdAudio(w, sig, argStr(argc, argv, "--out", "."), static_cast<float>(argNum(argc, argv, "--best", 15.0)),
                        std::strcmp(argStr(argc, argv, "--mode", "usb"), "am") == 0, argNum(argc, argv, "--start", 20.0),
                        argNum(argc, argv, "--dur", 30.0), static_cast<float>(argNum(argc, argv, "--gain", 0.0)));
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
