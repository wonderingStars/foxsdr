// Time-frequency box export (dsp/box_export.hpp): proof on GENERATED signals - noise (about 54 dB below a unit tone), a 37 Hz LO offset and a +20 ppm
// sample-rate error on every recording. Measured: pass-band ripple across the box, alias rejection of every tone that folds into it and of 200 random
// out-of-band tones, tone placement, time extent and filter-delay removal, consistency between two boxes of one recording, AM / FM / USB / LSB demodulation
// levels, the WAV / cf32 files read back, refusals, and cancel.
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define BX_PID _getpid()
#else
#include <unistd.h>
#define BX_PID getpid()
#endif

#include "dsp/box_export.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::dsp::boxexport;
using cascade::source::transport::TransportSource;

namespace {

constexpr double kFs = 240000.0;
constexpr double kFc = 7.1e6;
constexpr double kLoOffset = 37.0;       // the real LO is this far above the nominal centre
constexpr double kPpm = 20.0;            // the real sample clock runs this many ppm fast
constexpr double kNoise = 0.002;

// the frequency a tone defined at RF `f` really has on the nominal sample grid
double effFreq(double f) { return kFc + (f - kFc - kLoOffset) / (1.0 + kPpm * 1e-6); }

double gauss(std::uint64_t n, std::uint64_t seed, double* second) {
    auto mix = [](std::uint64_t z) { z += 0x9E3779B97F4A7C15ull; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); };
    const double u1 = (static_cast<double>(mix(n * 2 + seed) >> 11) + 0.5) / 9007199254740992.0;
    const double u2 = (static_cast<double>(mix(n * 2 + 1 + seed) >> 11) + 0.5) / 9007199254740992.0;
    const double r = std::sqrt(-2.0 * std::log(u1));
    *second = r * std::sin(2.0 * 3.14159265358979323846 * u2);
    return r * std::cos(2.0 * 3.14159265358979323846 * u2);
}

// a recording made by a function of (frame number, time on the real clock)
class FnSource final : public TransportSource {
public:
    using Fn = std::function<std::complex<double>(std::uint64_t, double)>;
    FnSource(std::uint64_t total, Fn fn, double rate = kFs, double noise = kNoise, double ppm = kPpm) : total_(total), fn_(std::move(fn)), noise_(noise), trueRate_(rate * (1.0 + ppm * 1e-6)) {}
    std::uint64_t totalFrames() const override { return total_; }
    bool seek(std::uint64_t f) override { if (f > total_) { return false; } pos_ = f; return true; }
    std::size_t read(cf* d, std::size_t n) override {
        const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, total_ - pos_));
        for (std::size_t i = 0; i < k; ++i) {
            const std::uint64_t f = pos_ + i;
            std::complex<double> v = fn_(f, static_cast<double>(f) / trueRate_);
            if (noise_ > 0.0) { double b = 0; const double a = gauss(f, 12345, &b); v += std::complex<double>(a, b) * noise_; }
            d[i] = cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        pos_ += k;
        return k;
    }
    std::uint64_t total_;
    std::uint64_t pos_ = 0;
    Fn fn_;
    double noise_, trueRate_;
};

// a complex tone at RF frequency f (amplitude a), as the recording's real clock and LO see it
FnSource::Fn tone(double f, double a = 1.0, double t0 = -1.0, double t1 = 1e18) {
    return [=](std::uint64_t, double t) {
        if (t < t0 || t > t1) { return std::complex<double>(0, 0); }
        const double ph = 2.0 * 3.14159265358979323846 * (f - kFc - kLoOffset) * t;
        return std::complex<double>(a * std::cos(ph), a * std::sin(ph));
    };
}

class MemSink final : public Sink {
public:
    bool begin(unsigned c, double r, std::uint64_t e) override { ch = c; rate = r; expected = e; began = true; v.reserve(static_cast<std::size_t>(e) * c); return true; }
    bool write(const float* d, std::size_t n) override { v.insert(v.end(), d, d + n * ch); return true; }
    unsigned ch = 0; double rate = 0; std::uint64_t expected = 0; bool began = false;
    std::vector<float> v;
};

BoxSpec baseSpec(double centreOffset, double bw, double t0, double t1, Mode m = Mode::Iq) {
    BoxSpec s;
    s.sampleRateHz = kFs; s.centerHz = kFc;
    s.t0 = t0; s.t1 = t1;
    s.loHz = kFc + centreOffset - bw / 2; s.hiHz = kFc + centreOffset + bw / 2;
    s.mode = m;
    s.format = m == Mode::Iq ? Format::WavFloat32 : Format::WavFloat32;
    return s;
}

std::complex<double> projectIq(const std::vector<float>& v, unsigned ch, double freq, double rate, std::size_t skip) {
    const std::size_t n = v.size() / ch;
    std::complex<double> acc(0, 0);
    std::size_t cnt = 0;
    for (std::size_t i = skip; i + skip < n; ++i) {
        const double ph = -2.0 * 3.14159265358979323846 * freq * static_cast<double>(i) / rate;
        const std::complex<double> x = ch == 2 ? std::complex<double>(v[2 * i], v[2 * i + 1]) : std::complex<double>(v[i], 0.0);
        acc += x * std::complex<double>(std::cos(ph), std::sin(ph));
        ++cnt;
    }
    return cnt ? acc / static_cast<double>(cnt) : acc;
}

double dB(double a) { return 20.0 * std::log10(std::max(a, 1e-12)); }

BoxResult run(const FnSource::Fn& fn, const BoxSpec& spec, MemSink& sink, std::uint64_t total = static_cast<std::uint64_t>(kFs * 0.6)) {
    FnSource src(total, fn);
    return exportBox(src, spec, sink);
}

void testPlan() {
    BoxSpec s = baseSpec(30000, 10000, 0.0, 0.5);
    Plan p = planExport(static_cast<std::uint64_t>(kFs * 0.6), s);
    CHECK(p.ok && p.info.decimation == 19 && std::fabs(p.info.outRateHz - kFs / 19) < 1e-9 && p.channels == 2);
    CHECK(p.outFrames == (static_cast<std::uint64_t>(kFs * 0.5) + 18) / 19);
    CHECK(p.info.taps % 2 == 1 && p.info.designAttenuationDb == 80.0 && !p.info.clipped);
    std::printf("plan: 10 kHz box in 240 kS/s: D=%u, out %.1f S/s, %u taps, ripple bound %.5f dB\n", p.info.decimation, p.info.outRateHz, p.info.taps, p.info.designRippleDb);
    // SSB needs 2.2 B
    BoxSpec ssb = baseSpec(20000, 3000, 0.0, 0.5, Mode::Usb);
    Plan ps = planExport(static_cast<std::uint64_t>(kFs * 0.6), ssb);
    CHECK(ps.ok && ps.info.outRateHz >= 6600.0 - 1e-9 && ps.channels == 1);
    // a narrow box in a wide recording keeps its transition band: the taps stay under the limit and the asked attenuation is reached
    BoxSpec narrow;
    narrow.sampleRateHz = 2.4e6; narrow.centerHz = 100e6; narrow.t0 = 0; narrow.t1 = 1; narrow.loHz = 100e6 - 250; narrow.hiHz = 100e6 + 250;
    Plan pn = planExport(2400000 * 2, narrow);
    CHECK(pn.ok && pn.info.taps <= kMaxTaps && pn.info.designAttenuationDb == 80.0);
    std::printf("plan: 500 Hz box in 2.4 MS/s: D=%u, out %.1f S/s, %u taps\n", pn.info.decimation, pn.info.outRateHz, pn.info.taps);
    CHECK(pn.info.outRateHz > 1500.0);
    // the box as wide as the band: D = 1, no filtering needed
    BoxSpec whole = baseSpec(0, kFs, 0.0, 0.2);
    Plan pw = planExport(static_cast<std::uint64_t>(kFs), whole);
    CHECK(pw.ok && pw.info.decimation == 1 && pw.info.taps == 1);
    // clipping to the band and the time span
    BoxSpec clip = baseSpec(0, 10000, -1.0, 99.0);
    clip.loHz = kFc - 1e6; clip.hiHz = kFc + 100;
    Plan pc = planExport(static_cast<std::uint64_t>(kFs * 2), clip);
    CHECK(pc.ok && pc.info.clipped && std::fabs(pc.info.boxLoHz - (kFc - kFs / 2)) < 1e-6 && pc.info.t0 == 0.0 && std::fabs(pc.info.t1 - 2.0) < 1e-9);
    // refusals
    struct Row { const char* what; std::function<void(BoxSpec&)> edit; std::uint64_t total; };
    const std::uint64_t T = static_cast<std::uint64_t>(kFs * 2);
    const std::vector<Row> rows = {
        {"NaN lo", [](BoxSpec& b) { b.loHz = std::nan(""); }, T}, {"inf hi", [](BoxSpec& b) { b.hiHz = HUGE_VAL; }, T},
        {"lo above hi", [](BoxSpec& b) { std::swap(b.loHz, b.hiHz); }, T}, {"lo equals hi", [](BoxSpec& b) { b.hiHz = b.loHz; }, T},
        {"t0 after t1", [](BoxSpec& b) { std::swap(b.t0, b.t1); }, T}, {"empty time", [](BoxSpec& b) { b.t1 = b.t0; }, T}, {"NaN time", [](BoxSpec& b) { b.t0 = std::nan(""); }, T},
        {"box above the band", [](BoxSpec& b) { b.loHz = kFc + 200000; b.hiHz = kFc + 210000; }, T},
        {"box below the band", [](BoxSpec& b) { b.loHz = kFc - 210000; b.hiHz = kFc - 200000; }, T},
        {"time after the end", [](BoxSpec& b) { b.t0 = 5; b.t1 = 6; }, T}, {"time before the start", [](BoxSpec& b) { b.t0 = -5; b.t1 = -1; }, T},
        {"zero rate", [](BoxSpec& b) { b.sampleRateHz = 0; }, T}, {"NaN rate", [](BoxSpec& b) { b.sampleRateHz = std::nan(""); }, T},
        {"attenuation low", [](BoxSpec& b) { b.attenuationDb = 39; }, T}, {"attenuation high", [](BoxSpec& b) { b.attenuationDb = 121; }, T},
        {"taps low", [](BoxSpec& b) { b.maxTaps = 30; }, T}, {"taps high", [](BoxSpec& b) { b.maxTaps = 8193; }, T},
        {"negative gain", [](BoxSpec& b) { b.audioGain = -1; }, T}, {"NaN deviation", [](BoxSpec& b) { b.fmDeviationHz = std::nan(""); }, T},
        {"iq as pcm16", [](BoxSpec& b) { b.format = Format::WavPcm16; }, T}, {"audio as cf32", [](BoxSpec& b) { b.mode = Mode::Am; b.format = Format::RawCf32; }, T},
        {"zero frames", [](BoxSpec&) {}, 0},
        {"over 4 GiB", [](BoxSpec& b) { b.loHz = kFc - kFs / 2; b.hiHz = kFc + kFs / 2; b.t0 = 0; b.t1 = 1e6; }, 240000ull * 1000000ull},
    };
    int n = 0;
    for (const Row& r : rows) {
        BoxSpec b = baseSpec(30000, 10000, 0.0, 1.0);
        r.edit(b);
        const Plan pr = planExport(r.total, b);
        if (pr.ok) { std::printf("NOT REFUSED: %s\n", r.what); }
        CHECK(!pr.ok && !pr.error.empty());
        ++n;
    }
    std::printf("plan refusal rows: %d\n", n);
    BoxSpec big = baseSpec(0, kFs, 0.0, 1.0e9);
    // the 4 GiB boundary: float32 stereo = 8 bytes a frame, 1.92 MB a second at 240 kS/s
    CHECK(planExport(static_cast<std::uint64_t>(kFs * 2100), big).ok);                     // 2100 s: 4.03e9 bytes, under the 0xFFFFF000 limit
    CHECK(!planExport(static_cast<std::uint64_t>(kFs * 2300), big).ok);                    // 2300 s: 4.42e9 bytes
    big.t1 = 2100.0;
    CHECK(planExport(static_cast<std::uint64_t>(kFs * 2300), big).ok);                     // only the box counts, not the recording
}

void testRipple() {
    const BoxSpec s = baseSpec(30000, 10000, 0.0, 0.5);
    const double f0 = kFc + 30000;
    std::vector<double> amps;
    MemSink sink;
    for (int k = -10; k <= 10; ++k) {
        const double b = k * 500.0;                                                      // -5000 .. +5000: the whole box
        sink = MemSink();
        const BoxResult r = run(tone(f0 + b), s, sink);
        CHECK(r.ok && r.framesWritten == sink.v.size() / 2);
        const double bm = effFreq(f0 + b) - f0;
        amps.push_back(std::abs(projectIq(sink.v, 2, bm, r.outRateHz, sink.v.size() / 2 / 20)));
    }
    const double mx = *std::max_element(amps.begin(), amps.end()), mn = *std::min_element(amps.begin(), amps.end());
    const double ripple = dB(mx) - dB(mn);
    std::printf("pass-band ripple over the whole 10 kHz box (21 tones, -5000..+5000 Hz): %.5f dB (max %.6f, min %.6f); design bound %.5f dB\n", ripple, mx, mn, 20 * std::log10(1 + 1e-4));
    CHECK(ripple < 0.01);
    CHECK(std::fabs(mx - 1.0) < 0.002 && std::fabs(mn - 1.0) < 0.002);
}

void testAliasRejection() {
    const BoxSpec s = baseSpec(30000, 10000, 0.0, 0.5);
    const double f0 = kFc + 30000;
    MemSink sink;
    BoxResult r0 = run(tone(f0 + 1000), s, sink);
    const double outRate = r0.outRateHz;
    const double ref = std::abs(projectIq(sink.v, 2, effFreq(f0 + 1000) - f0, outRate, sink.v.size() / 2 / 20));
    CHECK(std::fabs(ref - 1.0) < 0.002);
    double worstFold = -1000.0;
    int folds = 0;
    for (double b : {-4000.0, -1000.0, 1000.0, 3000.0, 4500.0}) {
        for (int k = -9; k <= 9; ++k) {
            if (k == 0) { continue; }
            const double fin = f0 + b + k * outRate;
            if (std::fabs(fin - kFc) > kFs / 2 - 3000) { continue; }
            sink = MemSink();
            const BoxResult r = run(tone(fin), s, sink);
            CHECK(r.ok);
            const double bin = effFreq(fin) - f0;
            double a = bin - outRate * std::round(bin / outRate);
            const double amp = std::abs(projectIq(sink.v, 2, a, outRate, sink.v.size() / 2 / 20));
            worstFold = std::max(worstFold, dB(amp / ref));
            ++folds;
        }
    }
    std::printf("alias rejection: %d tones that fold into the pass band, worst leak %.1f dB (design 80)\n", folds, worstFold);
    CHECK(folds > 40);
    CHECK(worstFold < -75.0);
    // 200 random tones anywhere out of the box's transition band: whatever they alias to, nothing above -75 dB
    std::mt19937 g(17);
    double worst = -1000.0;
    int tested = 0;
    for (int i = 0; i < 200; ++i) {
        const double fin = kFc - kFs / 2 + 3000 + (g() % 1000000) / 1000000.0 * (kFs - 6000);
        const double bin = effFreq(fin) - f0;
        if (std::fabs(bin) < 7700.0) { continue; }                                          // inside the pass or transition band: no promise
        sink = MemSink();
        const BoxResult r = run(tone(fin), s, sink);
        CHECK(r.ok);
        const double a = bin - outRate * std::round(bin / outRate);
        worst = std::max(worst, dB(std::abs(projectIq(sink.v, 2, a, outRate, sink.v.size() / 2 / 20)) / ref));
        ++tested;
    }
    std::printf("random out-of-band tones: %d tested, worst leak %.1f dB\n", tested, worst);
    CHECK(tested > 150 && worst < -75.0);
}

void testPlacementAndTime() {
    const double f0 = kFc + 30000;
    // placement of a tone 1234 Hz above the box centre
    {
        const BoxSpec s = baseSpec(30000, 10000, 0.0, 0.5);
        MemSink sink; const BoxResult r = run(tone(f0 + 1234), s, sink);
        std::complex<double> acc(0, 0);
        for (std::size_t i = 100; i + 1 < sink.v.size() / 2 - 100; ++i) { acc += std::complex<double>(sink.v[2 * i + 2], sink.v[2 * i + 3]) * std::conj(std::complex<double>(sink.v[2 * i], sink.v[2 * i + 1])); }
        const double fm = std::arg(acc) * r.outRateHz / (2 * 3.14159265358979323846);
        std::printf("placement: tone at box centre +1234 Hz appears at %.2f Hz (expected %.2f)\n", fm, effFreq(f0 + 1234) - f0);
        CHECK_NEAR(fm, effFreq(f0 + 1234) - f0, 0.5);
    }
    // time extent
    {
        BoxSpec s = baseSpec(30000, 10000, 0.1, 0.35);
        MemSink sink; const BoxResult r = run(tone(f0 + 500), s, sink);
        const std::uint64_t span = static_cast<std::uint64_t>(std::floor(0.35 * kFs + 0.5)) - static_cast<std::uint64_t>(std::floor(0.1 * kFs + 0.5));
        CHECK(r.ok && r.framesWritten == (span + 18) / 19 && sink.v.size() / 2 == r.framesWritten);
        CHECK(sink.expected == r.framesWritten && sink.ch == 2 && sink.began);
    }
    // a burst present only from 0.1 s to 0.35 s: the export of 0.05..0.4 s starts and stops where the burst does (the filter's delay is removed)
    {
        const BoxSpec s = baseSpec(30000, 10000, 0.05, 0.4);
        MemSink sink; const BoxResult r = run(tone(f0 + 500, 1.0, 0.1, 0.35), s, sink);
        const std::size_t n = sink.v.size() / 2;
        auto mag = [&](std::size_t i) { return std::hypot(static_cast<double>(sink.v[2 * i]), static_cast<double>(sink.v[2 * i + 1])); };
        // time of output frame i = 0.05 + i / outRate
        auto idx = [&](double t) { return static_cast<std::size_t>((t - 0.05) * r.outRateHz); };
        CHECK(mag(idx(0.09)) < 2e-3);                                                      // 10 ms before the burst: silence
        CHECK(std::fabs(mag(idx(0.12)) - 1.0) < 3e-3 && std::fabs(mag(idx(0.30)) - 1.0) < 3e-3);
        CHECK(mag(idx(0.37)) < 2e-3 && idx(0.37) < n);
        // half-amplitude points at the burst edges, to within two output samples
        std::size_t up = 0, down = 0;
        for (std::size_t i = 1; i < n; ++i) { if (up == 0 && mag(i - 1) < 0.5 && mag(i) >= 0.5) { up = i; } if (up != 0 && down == 0 && mag(i - 1) >= 0.5 && mag(i) < 0.5) { down = i; } }
        const double tUp = 0.05 + static_cast<double>(up) / r.outRateHz, tDown = 0.05 + static_cast<double>(down) / r.outRateHz;
        std::printf("burst edges: rise at %.5f s (true 0.1), fall at %.5f s (true 0.35); 2 output samples = %.5f s\n", tUp, tDown, 2.0 / r.outRateHz);
        CHECK(std::fabs(tUp - 0.1) < 2.0 / r.outRateHz && std::fabs(tDown - 0.35) < 2.0 / r.outRateHz);
    }
    // two boxes of one recording agree where they overlap (their starts are a whole number of decimation steps apart)
    {
        const double f1 = kFc + 31000;     // 31 kHz: the mixer phase between the two starts is NOT a whole number of cycles
        const BoxSpec a = baseSpec(31000, 10000, 0.0, 0.5);
        BoxSpec b = baseSpec(31000, 10000, 24016.0 / kFs, 0.4);
        MemSink sa, sb;
        const FnSource::Fn fn = [&](std::uint64_t n, double t) { return tone(f1 + 700)(n, t) + tone(f1 - 2300, 0.5)(n, t); };
        const BoxResult ra = run(fn, a, sa), rb = run(fn, b, sb);
        CHECK(ra.ok && rb.ok && ra.decimation == 19);
        const std::size_t off = 24016 / 19;
        double worst = 0;
        for (std::size_t i = 0; i + 1 < sb.v.size() / 2; ++i) { worst = std::max(worst, std::fabs(static_cast<double>(sa.v[2 * (i + off)]) - sb.v[2 * i])); worst = std::max(worst, std::fabs(static_cast<double>(sa.v[2 * (i + off) + 1]) - sb.v[2 * i + 1])); }
        std::printf("two boxes of one recording: worst difference %.3g\n", worst);
        CHECK(worst < 2e-5);
    }
}

void testDemod() {
    const double f0 = kFc + 30000;
    const double pi2 = 2.0 * 3.14159265358979323846;
    // AM: unit carrier, 1 kHz at 50 % depth -> audio amplitude 0.5
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.55, Mode::Am);
        const FnSource::Fn fn = [&](std::uint64_t, double t) {
            const double env = 1.0 + 0.5 * std::sin(pi2 * 1000.0 * t);
            const double ph = pi2 * (f0 - kFc - kLoOffset) * t;
            return std::complex<double>(env * std::cos(ph), env * std::sin(ph));
        };
        MemSink sink; const BoxResult r = run(fn, s, sink, static_cast<std::uint64_t>(kFs * 0.6));
        CHECK(r.ok && sink.ch == 1);
        const std::size_t skip = sink.v.size() / 4;
        const double amp = 2.0 * std::abs(projectIq(sink.v, 1, 1000.0 * (1.0 - kPpm * 1e-6), r.outRateHz, skip));
        double mean = 0; for (std::size_t i = skip; i < sink.v.size() - skip; ++i) { mean += sink.v[i]; } mean /= static_cast<double>(sink.v.size() - 2 * skip);
        std::printf("AM: audio amplitude %.4f (expected 0.5), mean %.4f\n", amp, mean);
        CHECK_NEAR(amp, 0.5, 0.01);
        CHECK(std::fabs(mean) < 0.01);
        CHECK(r.peak > 0.45f && r.peak < 1.5f);                                             // the start-up of the filter at the file's first frame is in the peak
    }
    // FM: +-2.5 kHz deviation at 1 kHz in a 10 kHz box -> audio amplitude 0.5; the LO offset shows as a DC step of -37/5000
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.55, Mode::Fm);
        const FnSource::Fn fn = [&](std::uint64_t, double t) {
            const double ph = pi2 * (f0 - kFc - kLoOffset) * t - (2500.0 / 1000.0) * std::cos(pi2 * 1000.0 * t);
            return std::complex<double>(std::cos(ph), std::sin(ph));
        };
        MemSink sink; const BoxResult r = run(fn, s, sink);
        const std::size_t skip = sink.v.size() / 8;
        const double amp = 2.0 * std::abs(projectIq(sink.v, 1, 1000.0 * (1.0 - kPpm * 1e-6), r.outRateHz, skip));
        double mean = 0; for (std::size_t i = skip; i < sink.v.size() - skip; ++i) { mean += sink.v[i]; } mean /= static_cast<double>(sink.v.size() - 2 * skip);
        std::printf("FM: audio amplitude %.4f (expected 0.5), DC %.5f (expected %.5f)\n", amp, mean, -kLoOffset / 5000.0);
        CHECK_NEAR(amp, 0.5, 0.01);
        CHECK_NEAR(mean, -kLoOffset / 5000.0, 0.002);
        // an explicit deviation rescales: 2500 Hz deviation with fmDeviationHz = 2500 gives +-1
        s.fmDeviationHz = 2500.0;
        MemSink s2; run(fn, s, s2);
        const double amp2 = 2.0 * std::abs(projectIq(s2.v, 1, 1000.0 * (1.0 - kPpm * 1e-6), r.outRateHz, skip));
        CHECK_NEAR(amp2, 1.0, 0.02);
        // gain applies last
        s.audioGain = 0.5;
        MemSink s3; run(fn, s, s3);
        CHECK_NEAR(2.0 * std::abs(projectIq(s3.v, 1, 1000.0 * (1.0 - kPpm * 1e-6), r.outRateHz, skip)), 0.5, 0.01);
    }
    // USB / LSB: a tone 1000 Hz below the box's upper edge, 2000 Hz above its lower edge (box 3 kHz wide)
    {
        const double c = kFc + 20000.0, bw = 3000.0;
        const double fTone = c + bw / 2 - 1000.0;
        for (Mode m : {Mode::Usb, Mode::Lsb}) {
            BoxSpec s = baseSpec(20000, bw, 0.0, 0.5, m);
            MemSink sink; const BoxResult r = run(tone(fTone), s, sink);
            CHECK(r.ok && r.outRateHz >= 2.2 * bw - 1e-9);
            const double expected = (m == Mode::Usb ? 2000.0 : 1000.0) + (effFreq(fTone) - fTone) * (m == Mode::Usb ? 1.0 : -1.0);
            const double other = (m == Mode::Usb ? 1000.0 : 2000.0);
            const std::size_t skip = sink.v.size() / 10;
            const double amp = 2.0 * std::abs(projectIq(sink.v, 1, expected, r.outRateHz, skip));
            const double wrong = 2.0 * std::abs(projectIq(sink.v, 1, other, r.outRateHz, skip));
            std::printf("%s: tone at %.2f Hz amplitude %.4f; nothing at the other sideband's %.0f Hz (%.5f)\n", m == Mode::Usb ? "USB" : "LSB", expected, amp, other, wrong);
            CHECK_NEAR(amp, 1.0, 0.02);
            CHECK(wrong < 0.01);
        }
    }
}

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
std::uint32_t rd32(const std::string& s, std::size_t o) { return static_cast<unsigned char>(s[o]) | (static_cast<unsigned char>(s[o + 1]) << 8) | (static_cast<unsigned char>(s[o + 2]) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[o + 3])) << 24); }
std::uint16_t rd16(const std::string& s, std::size_t o) { return static_cast<std::uint16_t>(static_cast<unsigned char>(s[o]) | (static_cast<unsigned char>(s[o + 1]) << 8)); }

void testFiles() {
    fs::path dir = fs::current_path() / ("recplay-box-" + std::to_string(BX_PID));
    fs::remove_all(dir); fs::create_directories(dir);
    const double f0 = kFc + 30000;
    const FnSource::Fn fn = [&](std::uint64_t n, double t) { return tone(f0 + 800)(n, t) * 3.0; };
    // I/Q float32 WAV
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.3);
        FnSource src(static_cast<std::uint64_t>(kFs * 0.6), fn);
        const BoxResult r = exportBoxToFile(src, s, dir / "iq.wav");
        MemSink m; run(fn, s, m);
        const std::string f = slurp(dir / "iq.wav");
        CHECK(r.ok && f.size() == 44 + r.framesWritten * 8 && f.compare(0, 4, "RIFF") == 0 && f.compare(8, 8, "WAVEfmt ") == 0 && rd16(f, 20) == 3 && rd16(f, 22) == 2 && rd16(f, 34) == 32);
        CHECK(rd32(f, 24) == static_cast<std::uint32_t>(std::floor(r.outRateHz + 0.5)) && rd32(f, 40) == r.framesWritten * 8 && rd32(f, 4) == f.size() - 8 && f.compare(36, 4, "data") == 0);
        CHECK(std::memcmp(f.data() + 44, m.v.data(), m.v.size() * 4) == 0);                  // the file holds exactly what the sink saw
        CHECK(rd16(f, 32) == 8 && rd32(f, 28) == rd32(f, 24) * 8);
    }
    // raw cf32
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.3); s.format = Format::RawCf32;
        FnSource src(static_cast<std::uint64_t>(kFs * 0.6), fn);
        const BoxResult r = exportBoxToFile(src, s, dir / "iq.cf32");
        CHECK(r.ok && fs::file_size(dir / "iq.cf32") == r.framesWritten * 8);
    }
    // PCM16 audio: rounding half away from zero, clipping at the rails
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.4, Mode::Am); s.format = Format::WavPcm16; s.audioGain = 1.0;
        const FnSource::Fn am = [&](std::uint64_t, double t) { return tone(f0)(0, t) * (1.0 + 0.5 * std::sin(2 * 3.14159265358979323846 * 1000.0 * t)); };
        for (double gain : {1.0, 100.0}) {
            s.audioGain = gain;
            FnSource src(static_cast<std::uint64_t>(kFs * 0.6), am);
            const BoxResult r = exportBoxToFile(src, s, dir / "am.wav");
            BoxSpec sf = s; sf.format = Format::WavFloat32;
            MemSink m; run(am, sf, m);
            const std::string f = slurp(dir / "am.wav");
            CHECK(r.ok && rd16(f, 20) == 1 && rd16(f, 22) == 1 && rd16(f, 34) == 16 && f.size() == 44 + r.framesWritten * 2 && rd32(f, 40) == r.framesWritten * 2);
            bool equal = true; int clipped = 0;
            for (std::size_t i = 0; i < m.v.size(); ++i) {
                const std::int16_t got = static_cast<std::int16_t>(rd16(f, 44 + 2 * i));
                const double sc = static_cast<double>(m.v[i]) * 32767.0;
                const double c = std::min(32767.0, std::max(-32768.0, sc));
                const std::int16_t want = static_cast<std::int16_t>(c >= 0 ? c + 0.5 : c - 0.5);
                equal = equal && got == want;
                if (got == 32767 || got == -32768) { ++clipped; }
            }
            CHECK(equal);
            if (gain == 100.0) { CHECK(clipped > 1000); } else { CHECK(clipped == 0); }
        }
    }
    // an unwritable path fails cleanly
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.3);
        FnSource src(static_cast<std::uint64_t>(kFs * 0.6), fn);
        const BoxResult r = exportBoxToFile(src, s, dir / "no-such-folder" / "x.wav");
        CHECK(!r.ok && !r.error.empty() && r.error.find("no-such-folder") == std::string::npos);
    }
    // cancel leaves a valid WAV of what was written
    {
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.5);
        FnSource src(static_cast<std::uint64_t>(kFs * 0.6), fn);
        std::atomic<bool> cancel{false};
        int calls = 0;
        const BoxResult r = exportBoxToFile(src, s, dir / "cancel.wav", &cancel, [&](double) { if (++calls == 1) { cancel = true; } });
        const std::string f = slurp(dir / "cancel.wav");
        CHECK(!r.ok && r.cancelled && r.framesWritten > 0 && r.framesWritten < 6000);
        CHECK(f.size() == 44 + r.framesWritten * 8 && rd32(f, 40) == r.framesWritten * 8);
        // cancelled before it starts
        std::atomic<bool> already{true};
        MemSink sink;
        FnSource src2(static_cast<std::uint64_t>(kFs * 0.6), fn);
        const BoxResult r2 = exportBox(src2, s, sink, &already);
        CHECK(!r2.ok && r2.cancelled && r2.framesWritten == 0);
    }
    // a source that fails mid-way
    {
        struct Dying final : TransportSource {
            std::uint64_t pos = 0;
            std::uint64_t totalFrames() const override { return 144000; }
            bool seek(std::uint64_t f) override { pos = f; return true; }
            std::size_t read(cf* d, std::size_t n) override { if (pos >= 70000) { return 0; } const std::size_t k = std::min<std::size_t>(n, 70000 - pos); for (std::size_t i = 0; i < k; ++i) { d[i] = cf(0.1f, 0.0f); } pos += k; return k; }
        } dying;
        BoxSpec s = baseSpec(30000, 10000, 0.0, 0.5);
        MemSink sink;
        const BoxResult r = exportBox(dying, s, sink);
        CHECK(!r.ok && !r.error.empty() && !r.cancelled);
    }
    // a source whose seek fails
    {
        struct NoSeek final : TransportSource {
            std::uint64_t totalFrames() const override { return 144000; }
            bool seek(std::uint64_t) override { return false; }
            std::size_t read(cf*, std::size_t) override { return 0; }
        } ns;
        BoxSpec s = baseSpec(30000, 10000, 0.1, 0.5);
        MemSink sink;
        CHECK(!exportBox(ns, s, sink).ok);
    }
    std::error_code ec; fs::remove_all(dir, ec);
}

void testWideRecording() {
    // 2.4 MS/s, a 12.5 kHz channel: the realistic case, with its speed
    const double fs = 2.4e6, fc = 100e6;
    BoxSpec s;
    s.sampleRateHz = fs; s.centerHz = fc; s.t0 = 0.2; s.t1 = 1.2;
    const double f0 = fc + 300000.0;
    s.loHz = f0 - 6250; s.hiHz = f0 + 6250;
    FnSource::Fn fn = [&](std::uint64_t, double t) {
        const double ph = 2.0 * 3.14159265358979323846 * (f0 + 2000.0 - fc - kLoOffset) * t;
        const double ph2 = 2.0 * 3.14159265358979323846 * (f0 + 12500.0 - fc - kLoOffset) * t;     // the next channel up (12.5 kHz spacing): outside the box and beyond the stop-band edge
        return std::complex<double>(std::cos(ph) + 0.5 * std::cos(ph2), std::sin(ph) + 0.5 * std::sin(ph2));
    };
    FnSource src(static_cast<std::uint64_t>(fs * 1.4), fn, fs, kNoise);
    MemSink sink;
    const auto t0 = std::chrono::steady_clock::now();
    const BoxResult r = exportBox(src, s, sink);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.ok);
    const double eff = fc + (f0 + 2000.0 - fc - kLoOffset) / (1.0 + kPpm * 1e-6);
    const double a = std::abs(projectIq(sink.v, 2, eff - f0, r.outRateHz, sink.v.size() / 2 / 20));
    const double effN = fc + (f0 + 12500.0 - fc - kLoOffset) / (1.0 + kPpm * 1e-6);
    const double bn = effN - f0;
    const double alias = bn - r.outRateHz * std::round(bn / r.outRateHz);
    const double n = std::abs(projectIq(sink.v, 2, alias, r.outRateHz, sink.v.size() / 2 / 20));
    std::printf("2.4 MS/s, 12.5 kHz box: D=%u, out %.1f S/s, %u taps, tone %.4f, next channel 12.5 kHz up %.1f dB, %.0f MS/s of input incl. generating it (%.2f s)\n", r.decimation, r.outRateHz,
                r.taps, a, dB(n / a), static_cast<double>(src.totalFrames()) / secs / 1e6, secs);
    CHECK_NEAR(a, 1.0, 0.003);
    CHECK(dB(n / a) < -75.0);
}

}  // namespace

int main() {
    testPlan();
    testRipple();
    testAliasRejection();
    testPlacementAndTime();
    testDemod();
    testFiles();
    testWideRecording();
    return testSummary("test_box_export");
}
