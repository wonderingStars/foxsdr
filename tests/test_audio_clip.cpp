// Reproduces a beta-tester report on 0.99.43: "when I widen the filter
// bandwidth in the spectrum view, the audio starts to crackle". The tester's
// own recording measured full-scale int16 clipping (6826 samples at
// +/-32767) that got markedly worse after widening the channel filter, with
// broadband 10-20 kHz energy and half-scale sample-to-sample jumps appearing
// in the widened section.
//
// WHAT THIS PROVES. A real AM carrier plus band-limited complex noise, pushed
// through the REAL VFO -> Demodulator -> Agc -> Squelch -> resampler ->
// Recorder chain (core/pipeline.cpp), first through a channel filter narrow
// enough to exclude the noise and then through one wide enough to admit all
// of it — exactly what "widening the filter bandwidth in the spectrum view"
// does (Vfo::setBandwidthHz, dsp/vfo.cpp). The noise is built from many
// independent-phase tones, which is the textbook way a bandwidth increase
// raises a signal's CREST FACTOR even when the Agc (dsp/agc.hpp) holds its
// RMS-ish level constant — high crest factor is exactly what a feedback Agc
// (reacting to the OUTPUT, one sample of lag behind) cannot fully tame, and
// what used to reach the int16 quantizer (core/recorder.cpp writeAudio) and
// the sound device (paNoFlag's hardware clip, sink/audio_out.cpp) with
// nothing in between to round the peaks off first.
//
// The recorder is the consumer measured here because it is what the tester's
// own evidence came from and because Recorder::writeAudio's int16 quantizer
// makes "hard-clipped" a literal, countable fact (a run of samples pinned at
// exactly +/-32767) rather than a judgement call about a float waveform.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/pipeline.hpp"
#include "core/recorder.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
// A real decimation (10x, well under Vfo::kStagedMinInputRateHz so the
// single-stage design in dsp/vfo.cpp's channelTaps applies): the channel
// filter's stopband edge sits at the 100 kHz channel Nyquist, which is what
// gives a 6 kHz-wide setting a real, steep rolloff instead of the near-flat
// response a 1:1 (undecimated) channel would produce for the same bandwidth
// number — exactly the shape the app's own VFO uses for a real radio.
constexpr double kInputRateHz = 2000000.0;
constexpr double kChannelRateHz = 200000.0;  // input / decim(10)
constexpr double kNarrowBwHz = 6000.0;   // passes the modulated carrier only
constexpr double kWideBwHz = 170000.0;   // passes the carrier AND every noise tone
constexpr double kCarrierAmp = 0.3;
constexpr double kModHz = 300.0;    // AM modulation tone: ordinary, low crest factor
constexpr double kModIndex = 0.7;
constexpr int kNoiseTones = 25;      // 40..76 kHz: inside the wide passband, and
                                     // far enough past the narrow filter's 3 kHz
                                     // cutoff (channel Nyquist 100 kHz sets the
                                     // far end of ITS transition too) to be well
                                     // attenuated there — see the file comment.
constexpr double kToneAmp = 0.1;

// A conventional AM-modulated carrier (crest factor ~1.6, the "fine at narrow
// bandwidth" baseline) plus a fixed comb of higher-frequency tones simulating
// adjacent-channel/wideband noise from 8 to 80 kHz — content a narrow channel
// filter excludes and a wide one admits whole, exactly what "widening the
// filter bandwidth in the spectrum view" does (Vfo::setBandwidthHz). Summing
// many mutually-incoherent tones is the textbook way to raise a signal's
// CREST FACTOR (central-limit effect) without raising its RMS by nearly as
// much — the property that defeats an Agc's near-target RMS regulation but
// not a peak limiter.
//
// Each tone is a phase-accumulator recurrence (one double complex multiply by
// a precomputed unit step per sample) rather than fresh cos/sin per sample:
// at 2 MS/s with ~20 tones the trig-per-sample cost would not keep up with
// the real-time pacing a free-running source is read under. The state is
// periodically renormalized to unit magnitude, so the few seconds this test
// runs for cannot accumulate visible drift.
class CarrierPlusNoiseSource : public cascade::source::IqSource {
public:
    CarrierPlusNoiseSource() {
        modRot_ = std::polar(1.0, kTwoPi * kModHz / kInputRateHz);
        modState_ = {1.0, 0.0};
        for (int k = 0; k < kNoiseTones; ++k) {
            const double f = 40000.0 + static_cast<double>(k) * 1500.0;
            toneRot_[static_cast<std::size_t>(k)] =
                std::polar(1.0, kTwoPi * f / kInputRateHz);
            // Golden-ratio initial phase per tone: deterministic (no RNG/seed
            // to manage) while keeping the tones mutually incoherent.
            const double phase0 =
                kTwoPi * (static_cast<double>(k + 1) * 0.6180339887498949);
            toneState_[static_cast<std::size_t>(k)] = std::polar(1.0, phase0);
        }
    }

    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kInputRateHz; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override { centerHz_ = hz; return true; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            modState_ *= modRot_;
            const double envelope = kCarrierAmp * (1.0 + kModIndex * modState_.real());
            std::complex<double> acc(envelope, 0.0);
            for (int k = 0; k < kNoiseTones; ++k) {
                auto& st = toneState_[static_cast<std::size_t>(k)];
                st *= toneRot_[static_cast<std::size_t>(k)];
                acc += kToneAmp * st;
            }
            dst[i] = std::complex<float>(static_cast<float>(acc.real()),
                                         static_cast<float>(acc.imag()));
            ++sample_;
            if ((sample_ & 0xFFFFu) == 0u) {
                // Renormalize every 65536 samples: cheap insurance against
                // the recurrence's magnitude drifting off 1.0 over a run.
                modState_ /= std::abs(modState_);
                for (auto& st : toneState_) { st /= std::abs(st); }
            }
        }
        return n;
    }

    const char* name() const override { return "carrier+noise test transmitter"; }
    const char* lastError() const override { return ""; }

private:
    std::complex<double> modRot_{1.0, 0.0};
    std::complex<double> modState_{1.0, 0.0};
    std::complex<double> toneRot_[static_cast<std::size_t>(kNoiseTones)];
    std::complex<double> toneState_[static_cast<std::size_t>(kNoiseTones)];
    std::uint64_t sample_ = 0;
    double centerHz_ = 100000000.0;
    bool running_ = false;
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

std::vector<std::int16_t> wavSamples(const std::string& path) {
    std::vector<std::int16_t> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) { return out; }
    f.seekg(44, std::ios::beg);  // canonical 44-byte header (core/recorder.cpp)
    std::int16_t s = 0;
    while (f.read(reinterpret_cast<char*>(&s), sizeof(s))) { out.push_back(s); }
    return out;
}

struct ClipStats {
    std::size_t count = 0;      // samples on this many
    std::int16_t peakAbs = 0;
    std::size_t longestRun = 0;  // longest run of consecutive samples pinned
                                 // at exactly the same full-scale value
    double rms = 0.0;
};

ClipStats measure(const std::vector<std::int16_t>& pcm) {
    ClipStats st;
    std::size_t run = 0;
    std::int16_t last = 0;
    double acc = 0.0;
    for (const std::int16_t s : pcm) {
        const std::int16_t a = static_cast<std::int16_t>(s < 0 ? -s : s);
        if (a > st.peakAbs) { st.peakAbs = a; }
        if (a >= 32767) {
            ++st.count;
            if (s == last) {
                ++run;
            } else {
                run = 1;
            }
            if (run > st.longestRun) { st.longestRun = run; }
        } else {
            run = 0;
        }
        last = s;
        acc += static_cast<double>(s) * static_cast<double>(s);
    }
    st.rms = pcm.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(pcm.size()));
    return st;
}

// Runs the live pipeline at the given VFO bandwidth for long enough for the
// Agc to settle (several hundred ms at its measured decay time constant —
// test_demod_agc.cpp pins that at 1/(decay*target) ~ 8000 samples ~ 170 ms)
// and records real time through the real Recorder, returning the recorded
// PCM. `pipeline` and `rec` are reused across calls so the Agc's settled
// state (not a fresh-construction transient) is what gets measured.
std::vector<std::int16_t> recordAt(cascade::core::Pipeline& pipeline,
                                   double bandwidthHz, double seconds,
                                   const std::string& dir) {
    pipeline.setVfoBandwidthHz(bandwidthHz);
    // Let the Agc react to the new spectrum before the recording that gets
    // measured starts, exactly as a tester hears the receiver AFTER moving
    // the bandwidth slider, not the instant they release it.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    cascade::core::Recorder rec;
    std::string err;
    if (!rec.start(cascade::core::RecordKind::Audio, dir,
                   cascade::core::Pipeline::kAudioRateHz, err)) {
        std::printf("  recorder start failed: %s\n", err.c_str());
        return {};
    }
    pipeline.setAudioRecorder(&rec);
    const std::uint64_t mark = pipeline.audioSamplesProduced();
    const std::uint64_t need =
        static_cast<std::uint64_t>(seconds * cascade::core::Pipeline::kAudioRateHz);
    waitFor([&] { return pipeline.audioSamplesProduced() - mark >= need; }, 30000);
    pipeline.setAudioRecorder(nullptr);
    rec.stop();

    std::string path;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".wav") { path = e.path().string(); }
    }
    if (path.empty()) { return {}; }
    return wavSamples(path);
}

}  // namespace

int main() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = kInputRateHz;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // headless: the recorder tap is what is measured
    cascade::core::Pipeline pipeline(cfg);
    pipeline.setDemodMode(cascade::dsp::DemodMode::AM);
    pipeline.setVfoOffsetHz(0.0);
    pipeline.setSquelchDb(-120.0f);  // never gating: nothing here should be silent
    pipeline.setVfoBandwidthHz(kNarrowBwHz);
    pipeline.setSource(std::make_unique<CarrierPlusNoiseSource>());
    pipeline.start();

    const std::string tag = std::to_string(TEST_GETPID());
    const std::string narrowDir = "audio_clip_" + tag + "_narrow";
    const std::string wideDir = "audio_clip_" + tag + "_wide";

    // --- Narrow filter: the carrier alone, comfortably below full scale -----
    const std::vector<std::int16_t> narrowPcm =
        recordAt(pipeline, kNarrowBwHz, 1.5, narrowDir);
    CHECK(narrowPcm.size() > 1000u);
    const ClipStats narrow = measure(narrowPcm);
    std::printf("narrow (%.0f Hz): samples=%zu peak=%d clipped=%zu longestRun=%zu rms=%.1f\n",
               kNarrowBwHz, narrowPcm.size(), static_cast<int>(narrow.peakAbs),
               narrow.count, narrow.longestRun, narrow.rms);
    CHECK(narrow.count == 0u);  // the baseline the tester describes as fine

    // --- Wide filter: the same carrier, now with every noise tone admitted --
    const std::vector<std::int16_t> widePcm =
        recordAt(pipeline, kWideBwHz, 1.5, wideDir);
    CHECK(widePcm.size() > 1000u);
    const ClipStats wide = measure(widePcm);
    std::printf("wide   (%.0f Hz): samples=%zu peak=%d clipped=%zu longestRun=%zu rms=%.1f\n",
               kWideBwHz, widePcm.size(), static_cast<int>(wide.peakAbs), wide.count,
               wide.longestRun, wide.rms);

    // THE DEFECT: widening the filter must not introduce hard clipping. Not
    // "fewer samples at full scale" — NONE, and no run of consecutive
    // full-scale samples at all (a single boundary sample landing on 32767
    // by rounding is not the crackle; a RUN of them is). This is the
    // assertion that is RED on the unfixed chain and GREEN once a limiter
    // guarantees the output stays inside full scale regardless of the input
    // signal's crest factor.
    CHECK(wide.count == 0u);
    CHECK(wide.longestRun == 0u);
    // Headroom, not just "not pinned": true peak should sit comfortably below
    // full scale (32767) — below dsp/limiter.hpp's kLimiterCeiling (0.99, i.e.
    // 32440), with slack for the quantizer's own rounding — matching the
    // recorder tap's own countable evidence rather than a float threshold no
    // one but the code ever sees.
    CHECK(wide.peakAbs < 32500);

    // THE LEVEL MUST NOT JUMP. The Agc's whole job is to hold the audio at a
    // fairly constant RMS loudness regardless of what the receiver is tuned
    // to see — a tester widening the filter should hear the same volume plus
    // crackle, not a level jump on top of it. Compared as a ratio rather than
    // an absolute figure because the AM envelope's own DC content already
    // sets the working level; 2x is generous headroom against the real
    // difference (n tones of comparable total noise power to the carrier
    // should raise RMS by a modest, bounded factor, not multiply it several
    // times over).
    CHECK(narrow.rms > 50.0);  // the reference itself is really producing audio
    const double ratio = wide.rms / narrow.rms;
    std::printf("level ratio wide/narrow = %.3f\n", ratio);
    CHECK(ratio < 2.0);
    CHECK(ratio > 0.5);

    pipeline.stop();

    if (g_checksFailed == 0) {
        std::error_code ec;
        std::filesystem::remove_all(narrowDir, ec);
        std::filesystem::remove_all(wideDir, ec);
    } else {
        std::printf("kept for inspection: %s %s\n", narrowDir.c_str(), wideDir.c_str());
    }

    return testSummary("test_audio_clip");
}
