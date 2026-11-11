// sigmeas_realdata - runs the sigmeas blocks on a real I/Q recording (2-channel WAV: PCM 8/16/32 bit
// or float32) and prints numbers only. A development tool of lane sigmeas, not a test and not part of
// the application: nothing is read except the file named on the command line, nothing is written
// except the numbers on stdout, nothing leaves the process.
//
//   sigmeas_realdata <wav> info
//   sigmeas_realdata <wav> obw   [startS durS [lowHz highHz]]
//   sigmeas_realdata <wav> burst [blockSamples [startS durS]]
//   sigmeas_realdata <wav> fsk   <centreHz> <widthHz> [startS durS [maxBaudHz]]
//   sigmeas_realdata <wav> ook   <centreHz> <widthHz> [startS durS [maxBaudHz]]
//   sigmeas_realdata <wav> hop   [startS durS]
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "dsp/sigmeas_bandwidth.hpp"
#include "dsp/sigmeas_bursts.hpp"
#include "dsp/sigmeas_hopchirp.hpp"
#include "dsp/sigmeas_symbols.hpp"
#include "dsp/sigmeas_vstime.hpp"

using namespace cascade::sigmeas;

namespace {

struct Wav {
    double rate = 0.0;
    int channels = 0, bits = 0, format = 0;
    std::uint64_t dataOffset = 0, dataBytes = 0;
};

bool readHeader(std::ifstream& f, Wav& w) {
    char id[4];
    std::uint32_t sz;
    f.read(id, 4);
    if (std::memcmp(id, "RIFF", 4) != 0) { return false; }
    f.read(reinterpret_cast<char*>(&sz), 4);
    f.read(id, 4);
    if (std::memcmp(id, "WAVE", 4) != 0) { return false; }
    bool haveFmt = false;
    while (f.read(id, 4)) {
        f.read(reinterpret_cast<char*>(&sz), 4);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<char> b(sz);
            f.read(b.data(), sz);
            std::uint16_t fmt, ch, bps;
            std::uint32_t sr;
            std::memcpy(&fmt, b.data(), 2);
            std::memcpy(&ch, b.data() + 2, 2);
            std::memcpy(&sr, b.data() + 4, 4);
            std::memcpy(&bps, b.data() + 14, 2);
            if (fmt == 0xFFFE && sz >= 26) { std::memcpy(&fmt, b.data() + 24, 2); }
            w.format = fmt;
            w.channels = ch;
            w.rate = sr;
            w.bits = bps;
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            w.dataOffset = static_cast<std::uint64_t>(f.tellg());
            w.dataBytes = sz;
            return haveFmt;
        } else {
            f.seekg(sz + (sz & 1u), std::ios::cur);
        }
    }
    return false;
}

// Reads [startS, startS + durS) (durS <= 0: to the end, at most maxSamples).
bool readIq(const std::string& path, const Wav& w, double startS, double durS, std::vector<Cf>& out,
            std::size_t maxSamples = 12000000) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    const std::size_t bytesPer = static_cast<std::size_t>(w.bits / 8) * static_cast<std::size_t>(w.channels);
    const std::uint64_t total = w.dataBytes / bytesPer;
    std::uint64_t first = static_cast<std::uint64_t>(std::max(0.0, startS) * w.rate);
    if (first >= total) { return false; }
    std::uint64_t count = durS > 0.0 ? static_cast<std::uint64_t>(durS * w.rate) : total - first;
    count = std::min<std::uint64_t>(std::min<std::uint64_t>(count, total - first), maxSamples);
    f.seekg(static_cast<std::streamoff>(w.dataOffset + first * bytesPer));
    std::vector<char> raw(static_cast<std::size_t>(count) * bytesPer);
    f.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    out.resize(static_cast<std::size_t>(count));
    double peak = 1e-30;
    std::vector<double> v(out.size() * 2);
    for (std::size_t i = 0; i < out.size() * 2; ++i) {
        const char* p = raw.data() + i * static_cast<std::size_t>(w.bits / 8);
        double s = 0.0;
        if (w.format == 3 && w.bits == 32) { float x; std::memcpy(&x, p, 4); s = x; }
        else if (w.bits == 16) { std::int16_t x; std::memcpy(&x, p, 2); s = x / 32768.0; }
        else if (w.bits == 8) { s = (static_cast<std::uint8_t>(*p) - 128) / 128.0; }
        else if (w.bits == 32) { std::int32_t x; std::memcpy(&x, p, 4); s = x / 2147483648.0; }
        else { return false; }
        v[i] = s;
        peak = std::max(peak, std::fabs(s));
    }
    // a float file can sit far above full scale: bring it to 0.9 so dBFS figures are comparable
    const double g = (w.format == 3 && peak > 1.0) ? 0.9 / peak : 1.0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = Cf(static_cast<float>(v[2 * i] * g), static_cast<float>(v[2 * i + 1] * g));
    }
    return true;
}

double arg(int argc, char** argv, int i, double def) {
    return i < argc ? std::atof(argv[i]) : def;
}

// The channel at centreHz, width widthHz, as complex baseband (amplitude and unwrapped phase of the
// vs-time block turned back into samples).
bool channel(const std::vector<Cf>& x, double rate, double centreHz, double widthHz, std::vector<Cf>& y, double& rateOut) {
    VsTimeParams p;
    p.centreHz = centreHz;
    p.bandwidthHz = widthHz;
    const VsTimeResult r = computeVsTime(x.data(), x.size(), rate, p);
    if (!r.valid) { std::printf("channel: %s\n", r.reason); return false; }
    y.resize(r.amplitude.size());
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double a = r.amplitude[i], ph = r.phaseRad[i];
        y[i] = Cf(static_cast<float>(a * std::cos(ph)), static_cast<float>(a * std::sin(ph)));
    }
    rateOut = r.rateOut;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: sigmeas_realdata <wav> info|obw|burst|fsk|ook|hop ...\n");
        return 2;
    }
    const std::string path = argv[1], cmd = argv[2];
    std::ifstream f(path, std::ios::binary);
    Wav w;
    if (!f || !readHeader(f, w)) { std::printf("cannot read WAV header\n"); return 1; }
    f.close();
    if (cmd == "info") {
        std::printf("rate %.0f channels %d bits %d format %d seconds %.3f\n", w.rate, w.channels, w.bits, w.format,
                    static_cast<double>(w.dataBytes) / static_cast<double>(w.channels * (w.bits / 8)) / w.rate);
        return 0;
    }
    if (w.channels != 2) { std::printf("not a 2-channel file\n"); return 1; }
    std::vector<Cf> x;
    if (cmd == "obw") {
        if (!readIq(path, w, arg(argc, argv, 3, 0.0), arg(argc, argv, 4, 0.0), x)) { std::printf("read failed\n"); return 1; }
        BandwidthParams p;
        if (argc > 6) { p.bandLowHz = arg(argc, argv, 5, 0.0); p.bandHighHz = arg(argc, argv, 6, 0.0); }
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), w.rate, p);
        std::printf("samples %zu valid %d reason '%s'\n", x.size(), r.valid ? 1 : 0, r.reason);
        if (r.valid) {
            std::printf("obw99 %.1f Hz  low %.1f high %.1f  centre %.1f  peak %.1f  snr %.1f dB  occupancy %.2f  bin %.2f Hz\n", r.obwHz,
                        r.obwLowHz, r.obwHighHz, r.centreHz, r.peakHz, r.snrDb, r.occupancy, r.binHz);
            for (const XdbEntry& e : r.xdb) { if (e.valid) { std::printf("  %.0f dB width %.1f Hz\n", e.db, e.widthHz); } }
        }
        return 0;
    }
    if (cmd == "burst") {
        if (!readIq(path, w, arg(argc, argv, 4, 0.0), arg(argc, argv, 5, 0.0), x)) { std::printf("read failed\n"); return 1; }
        BurstParams p;
        p.blockSamples = static_cast<std::size_t>(arg(argc, argv, 3, 0.0));
        const BurstReport r = detectBursts(x.data(), x.size(), w.rate, p);
        std::printf("samples %zu block %zu valid %d reason '%s' continuous %d\n", x.size(), r.blockSamples, r.valid ? 1 : 0, r.reason,
                    r.continuous ? 1 : 0);
        if (r.valid) {
            std::vector<double> len, pw;
            for (const Burst& b : r.bursts) { len.push_back(b.lengthS); pw.push_back(b.powerDbfs); }
            std::printf("bursts %zu  floor %.1f dBFS level %.1f dBFS  duty %.4f  repeat %.6f s jitter %.3f\n", r.totalBursts, r.noiseDbfs,
                        r.levelDbfs, r.dutyCycle, r.repeatPeriodS, r.repeatJitter);
            if (!len.empty()) {
                std::vector<double> a = len, b2 = len;
                std::printf("length min %.6f median %.6f max %.6f s\n", *std::min_element(a.begin(), a.end()),
                            percentileInPlace(b2, 0.5), *std::max_element(a.begin(), a.end()));
                // histogram of lengths in 8 us (or one block) classes
                const double cls = std::max(8e-6, static_cast<double>(r.blockSamples) / w.rate);
                std::vector<std::pair<long, int>> h;
                for (double l : len) {
                    const long c = std::lround(l / cls);
                    bool found = false;
                    for (auto& e : h) { if (e.first == c) { ++e.second; found = true; break; } }
                    if (!found) { h.push_back({c, 1}); }
                }
                std::sort(h.begin(), h.end(), [](const auto& p1, const auto& p2) { return p1.second > p2.second; });
                for (std::size_t i = 0; i < std::min<std::size_t>(5, h.size()); ++i) {
                    std::printf("  length class %.1f us : %d bursts\n", static_cast<double>(h[i].first) * cls * 1e6, h[i].second);
                }
            }
        }
        return 0;
    }
    if (cmd == "fsk" || cmd == "ook") {
        if (argc < 5) { std::printf("need centreHz widthHz\n"); return 1; }
        if (!readIq(path, w, arg(argc, argv, 5, 0.0), arg(argc, argv, 6, 0.0), x)) { std::printf("read failed\n"); return 1; }
        std::vector<Cf> y;
        double ro = 0.0;
        if (!channel(x, w.rate, arg(argc, argv, 3, 0.0), arg(argc, argv, 4, 0.0), y, ro)) { return 1; }
        SymbolParams p;
        p.maxBaudHz = arg(argc, argv, 7, 0.0);
        if (cmd == "fsk") {
            const FskResult r = analyseFsk(y.data(), y.size(), ro, p);
            std::printf("channel rate %.1f samples %zu valid %d reason '%s' separation %.2f edges %zu\n", ro, y.size(), r.valid ? 1 : 0,
                        r.reason, r.separation, r.edges);
            if (r.valid) {
                std::printf("baud %.2f  shift %.1f Hz  mark %.1f space %.1f centre %.1f  jitter %.3f UI  bits %zu\n", r.baudHz, r.shiftHz,
                            r.markHz, r.spaceHz, r.centreHz, r.jitterUi, r.bits.size());
            }
        } else {
            const OokResult r = sliceOok(y.data(), y.size(), ro, p);
            std::printf("channel rate %.1f samples %zu valid %d reason '%s' separation %.2f\n", ro, y.size(), r.valid ? 1 : 0, r.reason,
                        r.separation);
            if (r.valid) {
                std::printf("unit baud %.2f (period %.2f us)  pulses %zu  keyed %d short %.1f us long %.1f us  jitter %.3f UI\n", r.baudHz,
                            r.periodS * 1e6, r.pulseCount, r.pulseWidthKeyed ? 1 : 0, r.shortWidthS * 1e6, r.longWidthS * 1e6, r.jitterUi);
            }
        }
        return 0;
    }
    if (cmd == "hop") {
        if (!readIq(path, w, arg(argc, argv, 3, 0.0), arg(argc, argv, 4, 0.0), x)) { std::printf("read failed\n"); return 1; }
        const FreqTrack t = trackPeak(x.data(), x.size(), w.rate);
        std::printf("track valid %d ok %zu of %zu\n", t.valid ? 1 : 0, t.okCount, t.freqHz.size());
        const HopReport h = analyseHops(t);
        std::printf("hops valid %d reason '%s' dwells %zu rate %.2f Hz distinct %zu\n", h.valid ? 1 : 0, h.reason, h.dwells.size(), h.hopRateHz,
                    h.distinctFreqs.size());
        const ChirpReport c = analyseChirps(t);
        std::printf("chirp valid %d reason '%s' sweeps %zu slope %.1f Hz/s\n", c.valid ? 1 : 0, c.reason, c.sweeps.size(), c.slopeHzPerS);
        return 0;
    }
    std::printf("unknown command\n");
    return 2;
}
