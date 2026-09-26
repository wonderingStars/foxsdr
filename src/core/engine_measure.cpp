// engine_measure.cpp - see engine_measure.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/engine_measure.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "core/pipeline.hpp"
#include "core/plugin_runner.hpp"
#include "core/version.hpp"
#include "source/siggen_source.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace cascade::core {

namespace measure {

double tonePower(const float* x, std::size_t n, double freqHz, double rateHz) {
    if (x == nullptr || n == 0 || !(rateHz > 0.0)) { return 0.0; }
    const double w = 2.0 * 3.14159265358979323846 * freqHz / rateHz;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s0 = static_cast<double>(x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    // |X(k)|^2 for the bin, then scaled so a unit-amplitude sine reads its
    // mean square (0.5): |X| of a sine of amplitude A over n samples is A*n/2.
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    const double mag2 = re * re + im * im;
    const double nn = static_cast<double>(n);
    return 2.0 * mag2 / (nn * nn);
}

double dominantFrequencyHz(const float* x, std::size_t n, double rateHz, double loHz,
                           double hiHz, double stepHz) {
    if (!(stepHz > 0.0) || !(hiHz >= loHz)) { return loHz; }
    double best = loHz;
    double bestP = -1.0;
    for (double f = loHz; f <= hiHz + 1e-9; f += stepHz) {
        const double pw = tonePower(x, n, f, rateHz);
        if (pw > bestP) {
            bestP = pw;
            best = f;
        }
    }
    return best;
}

std::int64_t scanForCrossing(const float* tap, std::size_t n, std::uint64_t endIndex,
                             std::uint64_t& nextBlock, std::size_t block, double freqHz,
                             double rateHz, double threshold, bool& missed) {
    if (tap == nullptr || block == 0 || endIndex < n) { return -1; }
    const std::uint64_t first = endIndex - n;  // absolute index of tap[0]
    while (nextBlock + block <= endIndex) {
        if (nextBlock < first) {
            // This block's head has already been overwritten in the tap:
            // it can never be judged, so the figure for this retune is off.
            missed = true;
            nextBlock += block;
            continue;
        }
        const float* b = tap + static_cast<std::size_t>(nextBlock - first);
        const double pw = tonePower(b, block, freqHz, rateHz);
        nextBlock += block;
        if (pw >= threshold) { return static_cast<std::int64_t>(nextBlock); }
    }
    return -1;
}

}  // namespace measure

namespace {

constexpr std::size_t kTapFrames = 4096;   // Pipeline's audio tap length
constexpr std::size_t kBlock = 256;        // 5.3 ms at 48 kHz

std::int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Process CPU (user + kernel), seconds.
double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) { return 0.0; }
    const auto toS = [](const FILETIME& ft) {
        const ULONGLONG v = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return static_cast<double>(v) * 1e-7;
    };
    return toS(k) + toS(u);
#else
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) { return 0.0; }
    return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
           static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
#endif
}

// Working set and its peak, bytes.
void workingSet(std::uint64_t& now, std::uint64_t& peak) {
    now = 0;
    peak = 0;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        now = pmc.WorkingSetSize;
        peak = pmc.PeakWorkingSetSize;
    }
#else
    long pages = 0, resident = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) { resident = 0; }
        std::fclose(f);
    }
    now = static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) == 0) {
        peak = static_cast<std::uint64_t>(ru.ru_maxrss) * 1024u;  // KiB on Linux
    }
#endif
}

double envDouble(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') { return fallback; }
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end == v || !(d > 0.0)) { return fallback; }
    return d;
}

}  // namespace

std::unique_ptr<EngineMeasure> EngineMeasure::fromEnvironment() {
    const char* m = std::getenv("FOXSDR_MEASURE");
    if (m == nullptr || *m == '\0') { return nullptr; }
    std::unique_ptr<EngineMeasure> e(new EngineMeasure());
    if (std::strcmp(m, "run") == 0) {
        e->mode_ = Mode::Run;
    } else if (std::strcmp(m, "rates") == 0) {
        e->mode_ = Mode::Rates;
    } else if (std::strcmp(m, "latency") == 0) {
        e->mode_ = Mode::Latency;
    } else {
        std::fprintf(stderr, "cascade: FOXSDR_MEASURE='%s' is not run, rates or latency - ignored\n", m);
        return nullptr;
    }
    const char* out = std::getenv("FOXSDR_MEASURE_OUT");
    if (out == nullptr || *out == '\0') {
        std::fprintf(stderr, "cascade: FOXSDR_MEASURE needs FOXSDR_MEASURE_OUT - ignored\n");
        return nullptr;
    }
    e->outPath_ = out;
    e->seconds_ = envDouble("FOXSDR_MEASURE_SECONDS", 65.0);
    e->warmup_ = envDouble("FOXSDR_MEASURE_WARMUP", 5.0);
    e->rateHz_ = envDouble("FOXSDR_MEASURE_RATE", 2048000.0);
    e->window_ = envDouble("FOXSDR_MEASURE_WINDOW", 60.0);
    e->retunes_ = static_cast<int>(envDouble("FOXSDR_MEASURE_RETUNES", 50.0));
    if (const char* r = std::getenv("FOXSDR_MEASURE_RATES"); r != nullptr && *r != '\0') {
        const char* p = r;
        while (*p != '\0') {
            char* end = nullptr;
            const double hz = std::strtod(p, &end);
            if (end == p) { break; }
            if (hz > 0.0) { e->ladder_.push_back(hz); }
            p = end;
            while (*p == ',' || *p == ' ') { ++p; }
        }
    }
    if (e->mode_ == Mode::Rates && e->ladder_.empty()) {
        std::fprintf(stderr, "cascade: FOXSDR_MEASURE=rates needs FOXSDR_MEASURE_RATES - ignored\n");
        return nullptr;
    }
    std::fprintf(stderr, "cascade: measurement '%s' -> %s\n", m, e->outPath_.c_str());
    return e;
}

double EngineMeasure::now() const {
    return static_cast<double>(steadyNs() - t0Ns_) * 1e-9;
}

void EngineMeasure::installGenerator(Pipeline& p, const MeasureHooks& h, double rateHz,
                                     bool latencyTone) {
    // A generator of our own at the rate asked for, carrying the same demo
    // picture the application opens on (two tones over a noise floor) - or,
    // for the latency run, the tone moved to +100 kHz where the VFO will be
    // sent to find it. Pipeline::setSource is the ordinary source swap.
    auto gen = std::make_unique<cascade::source::SigGenSource>(rateHz);
    cascade::source::SigGen& g = gen->sigGen();
    g.setTone(0, latencyTone ? 100000.0 : 300000.0, -30.0f);
    g.setTone(1, -500000.0, -45.0f);
    g.setNoiseFloorDb(-90.0f);
    if (h.installSource) {
        h.installSource(std::move(gen));
    } else {
        p.setSource(std::move(gen));
        (void)p.setInputRateHz(rateHz);
    }
    if (p.inputRateHz() != rateHz) {
        std::fprintf(stderr, "cascade: measure: the chain refused %.0f Hz and runs at %.0f Hz\n",
                     rateHz, p.inputRateHz());
    }
}

std::size_t EngineMeasure::readTap(Pipeline& p, std::uint64_t& endIndex) {
    if (tap_.size() < kTapFrames) { tap_.assign(kTapFrames, 0.0f); }
    // The count and the tap are updated together under the DSP thread's
    // lock but read here in two calls; a block landing between them would
    // misplace the window by one block, so take it only when the count did
    // not move across the copy.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint64_t a = p.audioSamplesProduced();
        const std::size_t n = p.audioTap(tap_.data(), kTapFrames);
        const std::uint64_t b = p.audioSamplesProduced();
        if (a == b) {
            endIndex = a;
            return n;
        }
    }
    endIndex = 0;
    return 0;
}

bool EngineMeasure::tick(Pipeline& p, const MeasureHooks& h) {
    if (!started_) {
        started_ = true;
        t0Ns_ = steadyNs();
        if (!p.running() && h.startReceiver) { h.startReceiver(); }
    }
    switch (mode_) {
        case Mode::Run: return tickRun(p, h);
        case Mode::Rates: return tickRates(p, h);
        case Mode::Latency: return tickLatency(p, h);
    }
    return false;
}

bool EngineMeasure::tickRun(Pipeline& p, const MeasureHooks& h) {
    const double t = now();
    if (phase_ == 0) {
        installGenerator(p, h, rateHz_, false);
        if (h.setMode) { h.setMode("WFM"); }
        phase_ = 1;
        return true;
    }
    if (phase_ == 1 && t >= warmup_) {
        cpu0_ = processCpuSeconds();
        drop0_ = p.ringDroppedSamples();
        audio0_ = p.audioSamplesProduced();
        std::size_t active = 0;
        readDecoders(p, active, decAudio0_, decIq0_, nullptr);
        lastVfoHz_ = p.vfoOffsetHz();
        phaseAt_ = t;
        phase_ = 2;
        return true;
    }
    if (phase_ == 2) {
        ++ticks_;
        const double vfo = p.vfoOffsetHz();
        if (vfo != lastVfoHz_) { ++vfoChanges_; }
        lastVfoHz_ = vfo;
    }
    if (phase_ == 2 && t >= seconds_) {
        cpu1_ = processCpuSeconds();
        drop1_ = p.ringDroppedSamples();
        audio1_ = p.audioSamplesProduced();
        runWindow_ = t - phaseAt_;
        workingSet(workingSet_, peakWorkingSet_);
        readDecoders(p, decodersActive_, decAudio1_, decIq1_, &decoderStatusJson_);
        finish(p);
        return false;
    }
    return true;
}

void EngineMeasure::readDecoders(Pipeline& p, std::size_t& active, std::uint64_t& audioFed,
                                 std::uint64_t& iqFed, std::string* statusJson) const {
    active = 0;
    audioFed = 0;
    iqFed = 0;
    if (statusJson != nullptr) { statusJson->assign("[]"); }
    const PluginRunner* r = p.pluginRunner();
    if (r == nullptr) { return; }
    active = r->activeCount();
    audioFed = static_cast<std::uint64_t>(r->audioFramesFed());
    iqFed = static_cast<std::uint64_t>(r->iqFramesFed());
    if (statusJson == nullptr) { return; }
    std::string js = "[";
    bool first = true;
    for (const DecoderStatus& s : r->status()) {
        // A display name is the plugin author's text: escape what JSON
        // requires and drop the other control characters.
        std::string name;
        for (const char c : s.plugin) {
            if (c == '"' || c == '\\') {
                name += '\\';
                name += c;
            } else if (static_cast<unsigned char>(c) >= 0x20) {
                name += c;
            }
        }
        char row[64];
        std::snprintf(row, sizeof(row), "\", \"reason\": %d, \"running\": %s}",
                      static_cast<int>(s.reason),
                      s.reason == DecoderIdleReason::Running ? "true" : "false");
        js += first ? "{\"plugin\": \"" : ", {\"plugin\": \"";
        js += name;
        js += row;
        first = false;
    }
    js += "]";
    *statusJson = js;
}

bool EngineMeasure::tickRates(Pipeline& p, const MeasureHooks& h) {
    const double t = now();
    if (phase_ == 0) {
        if (h.setMode) { h.setMode("WFM"); }
        step_ = 0;
        installGenerator(p, h, ladder_[0], false);
        phaseAt_ = t;
        phase_ = 1;
        return true;
    }
    if (phase_ == 1 && t - phaseAt_ >= warmup_) {
        drop0_ = p.ringDroppedSamples();
        audio0_ = p.audioSamplesProduced();
        phaseAt_ = t;
        phase_ = 2;
        return true;
    }
    if (phase_ == 2 && t - phaseAt_ >= window_) {
        RateStep s;
        s.requestedHz = ladder_[step_];
        s.inputRateHz = p.inputRateHz();
        s.dropped = p.ringDroppedSamples() - drop0_;
        s.audioSamples = p.audioSamplesProduced() - audio0_;
        s.seconds = t - phaseAt_;
        steps_.push_back(s);
        std::fprintf(stderr, "cascade: measure rate %.0f Hz: %llu dropped in %.1f s\n",
                     s.requestedHz, static_cast<unsigned long long>(s.dropped), s.seconds);
        ++step_;
        if (s.dropped > 0 || step_ >= ladder_.size()) {
            finish(p);
            return false;
        }
        installGenerator(p, h, ladder_[step_], false);
        phaseAt_ = t;
        phase_ = 1;
    }
    return true;
}

bool EngineMeasure::tickLatency(Pipeline& p, const MeasureHooks& h) {
    const double t = now();
    const double rate = Pipeline::kAudioRateHz;
    switch (phase_) {
        case 0:
            installGenerator(p, h, rateHz_, true);
            if (h.setMode) { h.setMode("USB"); }
            if (h.tuneVfoHz) { h.tuneVfoHz(onOffsetHz_); }
            phaseAt_ = t;
            phase_ = 1;
            return true;
        case 1: {
            // Settled on the tone: find where the demodulator put it, and
            // what "steady" is.
            if (t - phaseAt_ < 2.0) { return true; }
            std::uint64_t end = 0;
            const std::size_t n = readTap(p, end);
            if (n < kTapFrames) {
                error_ = "audio tap not full after 2 s on the tone";
                finish(p);
                return false;
            }
            toneHz_ = measure::dominantFrequencyHz(tap_.data(), n, rate, 200.0, 3500.0, 5.0);
            double sum = 0.0;
            const std::size_t blocks = n / kBlock;
            for (std::size_t b = 0; b < blocks; ++b) {
                sum += measure::tonePower(tap_.data() + b * kBlock, kBlock, toneHz_, rate);
            }
            steady_ = sum / static_cast<double>(blocks);
            if (!(steady_ > 0.0)) {
                error_ = "no steady tone power";
                finish(p);
                return false;
            }
            std::fprintf(stderr, "cascade: measure latency: tone at %.0f Hz audio, steady %.3g\n",
                         toneHz_, steady_);
            latDrop0_ = p.ringDroppedSamples();
            latDropArmed_ = true;
            phase_ = 2;
            return true;
        }
        case 2:
            if (h.tuneVfoHz) { h.tuneVfoHz(awayOffsetHz_); }
            phaseAt_ = t;
            phase_ = 3;
            return true;
        case 3: {
            if (t - phaseAt_ < 0.5) { return true; }
            std::uint64_t end = 0;
            const std::size_t n = readTap(p, end);
            awayRatio_ = 0.0;
            for (std::size_t b = 0; b < 8 && (b + 1) * kBlock <= n; ++b) {
                const float* blk = tap_.data() + n - (b + 1) * kBlock;
                awayRatio_ = std::max(awayRatio_,
                                      measure::tonePower(blk, kBlock, toneHz_, rate) / steady_);
            }
            commandIndex_ = p.audioSamplesProduced();
            nextBlock_ = commandIndex_;
            commandNs_ = steadyNs();
            detected_ = false;
            results_.push_back(Retune{});
            results_.back().awayRatio = awayRatio_;
            if (h.tuneVfoHz) { h.tuneVfoHz(onOffsetHz_); }
            phaseAt_ = t;
            phase_ = 4;
            return true;
        }
        case 4: {
            Retune& r = results_.back();
            if (!detected_) {
                std::uint64_t end = 0;
                const std::size_t n = readTap(p, end);
                if (n > 0) {
                    bool missed = false;
                    const std::int64_t cross = measure::scanForCrossing(
                        tap_.data(), n, end, nextBlock_, kBlock, toneHz_, rate, 0.5 * steady_,
                        missed);
                    if (missed) { r.missed = true; }
                    if (cross >= 0) {
                        detected_ = true;
                        r.latencyMs = static_cast<double>(static_cast<std::uint64_t>(cross) -
                                                          commandIndex_) *
                                      1000.0 / rate;
                        r.wallMs = static_cast<double>(steadyNs() - commandNs_) * 1e-6;
                    }
                }
            }
            const double held = t - phaseAt_;
            if ((detected_ && held >= 0.3) || held >= 1.5) {
                if (static_cast<int>(results_.size()) >= retunes_) {
                    finish(p);
                    return false;
                }
                phase_ = 2;
            }
            return true;
        }
        default: break;
    }
    return true;
}

void EngineMeasure::finish(Pipeline& p) {
    std::FILE* f = std::fopen(outPath_.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cascade: measurement result %s could not be written\n",
                     outPath_.c_str());
        return;
    }
    const char* modeName = mode_ == Mode::Run ? "run" : (mode_ == Mode::Rates ? "rates" : "latency");
    // /2 (engine step 1a repair): run results carry the decoders fed and the
    // VFO changes, latency results the input rate and the ring drops, so
    // tools/measure_engine.ps1 can refuse a run that did less than it says.
    std::fprintf(f, "{\n  \"format\": \"foxsdr-measure/2\",\n");
    std::fprintf(f, "  \"version\": \"%s\",\n  \"commit\": \"%s\",\n", cascade::versionString(),
                 cascade::gitCommit());
    std::fprintf(f, "  \"mode\": \"%s\",\n", modeName);
    std::fprintf(f, "  \"error\": \"%s\",\n", error_.c_str());
    std::fprintf(f, "  \"faulted\": %s,\n", p.faulted() ? "true" : "false");
    if (mode_ == Mode::Run) {
        std::fprintf(f, "  \"rateHz\": %.0f,\n  \"inputRateHz\": %.0f,\n", rateHz_, p.inputRateHz());
        std::fprintf(f, "  \"warmupS\": %.3f,\n  \"windowS\": %.3f,\n", warmup_, runWindow_);
        std::fprintf(f, "  \"cpuS\": %.6f,\n", cpu1_ - cpu0_);
        std::fprintf(f, "  \"ringDropped\": %llu,\n",
                     static_cast<unsigned long long>(drop1_ - drop0_));
        std::fprintf(f, "  \"audioSamples\": %llu,\n",
                     static_cast<unsigned long long>(audio1_ - audio0_));
        std::fprintf(f, "  \"workingSetBytes\": %llu,\n  \"peakWorkingSetBytes\": %llu,\n",
                     static_cast<unsigned long long>(workingSet_),
                     static_cast<unsigned long long>(peakWorkingSet_));
        // Fed across the window. A rebuild inside it (none is expected: the
        // source is swapped before the warm-up) restarts the runner's count,
        // and then only the part after it is known.
        const auto across = [](std::uint64_t a, std::uint64_t b) { return b >= a ? b - a : b; };
        std::fprintf(f, "  \"decodersActive\": %zu,\n", decodersActive_);
        std::fprintf(f, "  \"decoderAudioFramesFed\": %llu,\n  \"decoderIqFramesFed\": %llu,\n",
                     static_cast<unsigned long long>(across(decAudio0_, decAudio1_)),
                     static_cast<unsigned long long>(across(decIq0_, decIq1_)));
        std::fprintf(f, "  \"decoders\": %s,\n", decoderStatusJson_.c_str());
        std::fprintf(f, "  \"ticks\": %llu,\n  \"vfoChanges\": %llu\n",
                     static_cast<unsigned long long>(ticks_),
                     static_cast<unsigned long long>(vfoChanges_));
    } else if (mode_ == Mode::Rates) {
        std::fprintf(f, "  \"warmupS\": %.3f,\n  \"windowS\": %.3f,\n  \"steps\": [", warmup_,
                     window_);
        for (std::size_t i = 0; i < steps_.size(); ++i) {
            const RateStep& s = steps_[i];
            std::fprintf(f,
                         "%s\n    {\"requestedHz\": %.0f, \"inputRateHz\": %.0f, \"dropped\": %llu, "
                         "\"audioSamples\": %llu, \"seconds\": %.3f}",
                         i == 0 ? "" : ",", s.requestedHz, s.inputRateHz,
                         static_cast<unsigned long long>(s.dropped),
                         static_cast<unsigned long long>(s.audioSamples), s.seconds);
        }
        std::fprintf(f, "\n  ]\n");
    } else {
        std::fprintf(f, "  \"rateHz\": %.0f,\n  \"inputRateHz\": %.0f,\n", rateHz_,
                     p.inputRateHz());
        std::fprintf(f, "  \"ringDropped\": %llu,\n",
                     static_cast<unsigned long long>(
                         latDropArmed_ ? p.ringDroppedSamples() - latDrop0_ : 0u));
        std::fprintf(f, "  \"toneAudioHz\": %.1f,\n  \"steadyPower\": %.6g,\n", toneHz_, steady_);
        std::fprintf(f, "  \"blockSamples\": %zu,\n  \"retunes\": [", kBlock);
        for (std::size_t i = 0; i < results_.size(); ++i) {
            const Retune& r = results_[i];
            std::fprintf(f,
                         "%s\n    {\"latencyMs\": %.3f, \"wallMs\": %.3f, \"awayRatio\": %.4f, "
                         "\"missed\": %s}",
                         i == 0 ? "" : ",", r.latencyMs, r.wallMs, r.awayRatio,
                         r.missed ? "true" : "false");
        }
        std::fprintf(f, "\n  ]\n");
    }
    std::fprintf(f, "}\n");
    std::fclose(f);
    std::fprintf(stderr, "cascade: measurement written to %s\n", outPath_.c_str());
}

}  // namespace cascade::core
