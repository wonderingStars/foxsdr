// Tests for the SOUND CARD source (source/soundcard_source.hpp), its real-to-
// complex conversion (dsp/real_to_iq.hpp) and the Source section's rules for
// it (gui/soundcard_panel.hpp, the remembered-source rule in tune_control.hpp,
// the config fields).
//
// NO SOUND CARD IS USED. The source is handed a FAKE backend that lists
// scripted devices and pushes scripted frames through exactly the callback
// path PortAudio drives, so ctest needs no audio hardware. The one section
// that talks to the real PortAudio only ENUMERATES (nothing is opened) and
// asserts nothing about what it finds: it is there to prove the backend runs
// on each platform's host API, and it prints what it saw.
//
// THE HEADLINE MEASUREMENT is the tester's own case: SAQ on 17.2 kHz through a
// card at 192 kHz. A synthetic 17.2 kHz tone on the card must land at 17.2 kHz
// on the spectrum's air axis (0 - 96 kHz), and a USB receiver on 16.4 kHz must
// turn it into an 800 Hz audio tone - measured at the far end of the real
// pipeline, not asserted about the converter alone. The same receiver on
// 17.6 kHz must NOT hear it (the tone is then below the dial, the lower
// sideband), which is what proves the spectrum is not mirrored.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
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

#include "core/config.hpp"
#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "dsp/fft.hpp"
#include "dsp/real_to_iq.hpp"
#include "gui/device_scan_plan.hpp"
#include "gui/soundcard_panel.hpp"
#include "gui/tune_control.hpp"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::source::SoundCardBackend;
using cascade::source::SoundCardDevice;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardRate;
using cascade::source::SoundCardSettings;
using cascade::source::SoundCardSource;
using Clock = std::chrono::steady_clock;

constexpr double kTwoPi = 6.283185307179586476925286766559;

// --- the fake backend -------------------------------------------------------------

// Fills `frames` interleaved frames starting at absolute frame `first`.
using Generator = std::function<void(float* out, std::size_t frames, std::uint64_t first, int ch)>;

class FakeBackend final : public SoundCardBackend {
public:
    std::vector<SoundCardDevice> devices;
    std::atomic<bool> aliveFlag{true};
    std::atomic<int> openCalls{0};
    std::atomic<int> closeCalls{0};
    int lastIndex = -1;
    int lastChannels = 0;
    double lastRate = 0.0;
    bool lastExclusive = false;
    Generator gen;  // set -> a real-time feeding thread runs while open

    ~FakeBackend() override { close(); }

    std::vector<SoundCardDevice> listDevices() override { return devices; }

    bool open(const SoundCardDevice& dev, int channels, double rateHz, bool exclusive, PushFn push,
              void* user, std::string& error) override {
        close();
        ++openCalls;
        lastIndex = dev.index;
        lastChannels = channels;
        lastRate = rateHz;
        lastExclusive = exclusive;
        if (failOpen) {
            error = "fake refused";
            return false;
        }
        push_ = push;
        user_ = user;
        isOpen_ = true;
        if (gen) {
            feeding_ = true;
            feeder_ = std::thread([this] { feedLoop(); });
        }
        return true;
    }

    void close() override {
        stopFeeding();
        if (isOpen_) { ++closeCalls; }
        isOpen_ = false;
    }

    bool alive() override { return isOpen_ && aliveFlag.load(); }

    // The card goes quiet (unplugged): no more callbacks.
    void stopFeeding() {
        feeding_ = false;
        if (feeder_.joinable()) { feeder_.join(); }
    }

    // One scripted block through the SAME callback a stream would call.
    void push(const float* interleaved, std::size_t frames) {
        if (push_ != nullptr) { push_(user_, interleaved, frames); }
    }

    bool failOpen = false;

private:
    void feedLoop() {
        const std::size_t block = static_cast<std::size_t>(lastRate / 100.0);  // 10 ms
        std::vector<float> buf(block * static_cast<std::size_t>(lastChannels));
        std::uint64_t frame = 0;
        auto next = Clock::now();
        while (feeding_) {
            gen(buf.data(), block, frame, lastChannels);
            frame += block;
            push(buf.data(), block);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }

    PushFn push_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> isOpen_{false};
    std::atomic<bool> feeding_{false};
    std::thread feeder_;
};

SoundCardDevice stereoCard(const std::string& name, const std::string& api, int index,
                           std::vector<SoundCardRate> rates, int channels = 2) {
    SoundCardDevice d;
    d.index = index;
    d.name = name;
    d.hostApi = api;
    d.maxInputChannels = channels;
    d.defaultRateHz = 48000.0;
    d.rates = std::move(rates);
    return d;
}

std::vector<SoundCardRate> ratesUpTo192k() {
    return {{8000.0, false},  {11025.0, false}, {16000.0, false}, {44100.0, false},
            {48000.0, false}, {96000.0, false}, {192000.0, true}};
}

SoundCardSource makeSource(const std::shared_ptr<FakeBackend>& fake) {
    return SoundCardSource([fake] { return std::static_pointer_cast<SoundCardBackend>(fake); });
}

// --- measurement helpers ----------------------------------------------------------

struct Peak {
    double hz = 0.0;       // baseband, signed
    double powerDb = -300.0;
};

// Hann-windowed FFT of the LAST 16384 samples of x (complex rate `rate`);
// returns the strongest bin's baseband frequency and the power (dB) at any
// other baseband frequency asked for.
constexpr std::size_t kFft = 16384;

std::vector<double> powerSpectrum(const std::vector<std::complex<float>>& x) {
    std::vector<std::complex<float>> in(kFft), out(kFft);
    const std::size_t off = x.size() - kFft;
    for (std::size_t i = 0; i < kFft; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / kFft);
        in[i] = x[off + i] * static_cast<float>(w);
    }
    cascade::dsp::ComplexFFT fft(kFft);
    fft.forward(in.data(), out.data());
    std::vector<double> p(kFft);
    for (std::size_t k = 0; k < kFft; ++k) { p[k] = std::norm(out[k]) + 1e-30; }
    return p;
}

double binHz(std::size_t k, double rate) {
    const double kk = (k < kFft / 2) ? static_cast<double>(k) : static_cast<double>(k) - kFft;
    return kk * rate / kFft;
}

Peak strongest(const std::vector<double>& p, double rate) {
    Peak best;
    std::size_t at = 0;
    for (std::size_t k = 0; k < p.size(); ++k) {
        if (p[k] > p[at]) { at = k; }
    }
    best.hz = binHz(at, rate);
    best.powerDb = 10.0 * std::log10(p[at]);
    return best;
}

// Strongest power within +/-3 bins of a baseband frequency, dB.
double powerNearDb(const std::vector<double>& p, double hz, double rate) {
    const long c = static_cast<long>(std::lround(hz / rate * kFft));
    double best = 1e-30;
    for (long d = -3; d <= 3; ++d) {
        long k = c + d;
        k = ((k % static_cast<long>(kFft)) + static_cast<long>(kFft)) % static_cast<long>(kFft);
        best = std::max(best, p[static_cast<std::size_t>(k)]);
    }
    return 10.0 * std::log10(best);
}

// Total power within +/-4 bins of a baseband frequency (the whole Hann main
// lobe, wherever between bins the tone falls), dB.
double energyNearDb(const std::vector<double>& p, double hz, double rate) {
    const long c = static_cast<long>(std::lround(hz / rate * kFft));
    double sum = 1e-30;
    for (long d = -4; d <= 4; ++d) {
        long k = c + d;
        k = ((k % static_cast<long>(kFft)) + static_cast<long>(kFft)) % static_cast<long>(kFft);
        sum += p[static_cast<std::size_t>(k)];
    }
    return 10.0 * std::log10(sum);
}

// Read everything the source will give until it has nothing for one read.
std::vector<std::complex<float>> drain(SoundCardSource& src, std::size_t want) {
    std::vector<std::complex<float>> out;
    std::vector<std::complex<float>> buf(2048);
    while (out.size() < want) {
        const std::size_t got = src.read(buf.data(), buf.size());
        if (got == 0) { break; }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

// Pushes `frames` frames of a generator through the fake in 1024-frame
// blocks, the way a card's callback arrives.
void pushGenerated(FakeBackend& fake, int ch, std::size_t frames, const Generator& g) {
    std::vector<float> buf(1024 * static_cast<std::size_t>(ch));
    std::uint64_t at = 0;
    while (at < frames) {
        const std::size_t n = std::min<std::size_t>(1024, frames - static_cast<std::size_t>(at));
        g(buf.data(), n, at, ch);
        fake.push(buf.data(), n);
        at += n;
    }
}

// --- 1. the conversion alone -----------------------------------------------------

void testRealToIqAxis() {
    // Two tones in one real signal: SAQ's 17.2 kHz, and 60 kHz in the upper
    // half of the band, at different levels so they cannot be confused.
    const double fs = 192000.0;
    const double outRate = cascade::dsp::RealToIq::outputRateHz(fs);
    const double centre = cascade::dsp::RealToIq::centreHz(fs);
    CHECK_NEAR(outRate, 96000.0, 0.0);
    CHECK_NEAR(centre, 48000.0, 0.0);

    const std::size_t n = 2 * (kFft + 4096);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(0.5 * std::cos(kTwoPi * 17200.0 * t + 0.3) +
                                  0.05 * std::cos(kTwoPi * 60000.0 * t + 1.1));
    }
    cascade::dsp::RealToIq conv;
    std::vector<std::complex<float>> z(cascade::dsp::RealToIq::outputCapacity(n));
    const std::size_t m = conv.process(x.data(), n, z.data());
    CHECK(m == n / 2);  // 2m real samples give exactly m outputs
    z.resize(m);

    const std::vector<double> p = powerSpectrum(z);
    const Peak pk = strongest(p, outRate);
    // THE AXIS: baseband + centre is the air frequency.
    CHECK_NEAR(centre + pk.hz, 17200.0, outRate / kFft);
    std::printf("real->iq: 17.2 kHz tone lands at %.1f Hz on the air axis\n", centre + pk.hz);
    // The two LEVELS as the energy under each tone's window lobe, so neither
    // is shortened by where it happens to fall between FFT bins (up to 1.4 dB
    // with a Hann window - which is what a peak-bin comparison measured).
    const double at60 = energyNearDb(p, 60000.0 - centre, outRate);
    const double at17 = energyNearDb(p, 17200.0 - centre, outRate);
    CHECK_NEAR(at60 - at17, -20.0, 0.1);  // 0.05 vs 0.5: -20 dB, and on the right side
    // THE MIRRORS, where a spectrum that kept the negative frequencies (or
    // shifted the wrong way) would put them: -17.2 kHz folds to air 78.8 kHz,
    // -60 kHz to air 36 kHz.
    const double mirror17 = powerNearDb(p, (fs / 2.0 - 17200.0) - centre, outRate);
    const double mirror60 = powerNearDb(p, (fs / 2.0 - 60000.0) - centre, outRate);
    std::printf("real->iq: mirror of 17.2 kHz at %.1f dB, of 60 kHz at %.1f dB (re the tone)\n",
                mirror17 - pk.powerDb, mirror60 - pk.powerDb);
    CHECK(mirror17 - pk.powerDb < -80.0);
    CHECK(mirror60 - pk.powerDb < -80.0);

    // THE LEVEL: a real tone of amplitude 0.5 is a complex tone of 0.5 (the
    // header's factor of 2). Measured as the mean |z| in the settled part of
    // a single-tone run.
    {
        cascade::dsp::RealToIq c1;
        std::vector<float> s(40000);
        for (std::size_t i = 0; i < s.size(); ++i) {
            s[i] = static_cast<float>(0.5 * std::cos(kTwoPi * 17200.0 * static_cast<double>(i) / fs));
        }
        std::vector<std::complex<float>> o(cascade::dsp::RealToIq::outputCapacity(s.size()));
        const std::size_t got = c1.process(s.data(), s.size(), o.data());
        double sum = 0.0;
        std::size_t cnt = 0;
        for (std::size_t i = 1000; i < got; ++i, ++cnt) { sum += std::abs(o[i]); }
        CHECK_NEAR(sum / static_cast<double>(cnt), 0.5, 0.01);
    }

    // BLOCKS DO NOT MATTER: any split gives bit-for-bit one call's output.
    {
        cascade::dsp::RealToIq a;
        cascade::dsp::RealToIq b;
        std::vector<std::complex<float>> whole(cascade::dsp::RealToIq::outputCapacity(n));
        const std::size_t wn = a.process(x.data(), n, whole.data());
        std::vector<std::complex<float>> parts;
        const std::size_t sizes[] = {7, 13, 1000, 1, 2, 4096};
        std::size_t at = 0;
        std::size_t si = 0;
        std::vector<std::complex<float>> tmp(8192);
        while (at < n) {
            const std::size_t len = std::min(sizes[si++ % 6], n - at);
            const std::size_t got = b.process(x.data() + at, len, tmp.data());
            parts.insert(parts.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
            at += len;
        }
        CHECK(parts.size() == wn);
        bool same = parts.size() == wn;
        for (std::size_t i = 0; same && i < wn; ++i) { same = (parts[i] == whole[i]); }
        CHECK(same);
    }
}

// --- 2. the source: real mode, channel choice ---------------------------------------

Generator twoChannelTones(double fs, double leftHz, double rightHz) {
    return [=](float* out, std::size_t frames, std::uint64_t first, int ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            const double t = static_cast<double>(first + i) / fs;
            out[i * static_cast<std::size_t>(ch)] = static_cast<float>(0.4 * std::cos(kTwoPi * leftHz * t));
            if (ch > 1) {
                out[i * static_cast<std::size_t>(ch) + 1] =
                    static_cast<float>(0.4 * std::cos(kTwoPi * rightHz * t));
            }
        }
    };
}

void testRealModeChannel() {
    for (int channel = 0; channel < 2; ++channel) {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 7, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        s.cardRateHz = 192000.0;
        s.format = SoundCardFormat::RealMono;
        s.channel = channel;
        CHECK(src.openWith(s));
        CHECK(src.start());
        CHECK_NEAR(src.sampleRateHz(), 96000.0, 0.0);
        CHECK_NEAR(src.centerFrequencyHz(), 48000.0, 0.0);
        CHECK(fake->lastChannels == 2);
        pushGenerated(*fake, 2, 2 * (kFft + 2048), twoChannelTones(192000.0, 17200.0, 40000.0));
        const auto z = drain(src, kFft + 2048);
        CHECK(z.size() >= kFft + 1024);
        if (z.size() < kFft) { continue; }
        const Peak pk = strongest(powerSpectrum(z), 96000.0);
        const double want = channel == 0 ? 17200.0 : 40000.0;
        std::printf("real mode, %s channel: tone at %.1f Hz\n", channel == 0 ? "left" : "right",
                    48000.0 + pk.hz);
        CHECK_NEAR(48000.0 + pk.hz, want, 96000.0 / kFft);
        src.closeDevice();
    }
}

// --- 3. I/Q mode: channel order and swap ---------------------------------------------

void testIqModeAndSwap() {
    const double fs = 48000.0;
    const double f = 6000.0;
    // A POSITIVE-frequency complex tone as an I/Q receiver delivers it:
    // I = cos, Q = sin on left and right.
    const Generator iq = [=](float* out, std::size_t frames, std::uint64_t first, int ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            const double ph = kTwoPi * f * static_cast<double>(first + i) / fs;
            out[i * static_cast<std::size_t>(ch)] = static_cast<float>(0.3 * std::cos(ph));
            out[i * static_cast<std::size_t>(ch) + 1] = static_cast<float>(0.3 * std::sin(ph));
        }
    };
    for (int swap = 0; swap < 2; ++swap) {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("SoftRock", "ALSA", 3, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "SoftRock";
        s.hostApi = "ALSA";
        s.cardRateHz = fs;
        s.format = SoundCardFormat::IqStereo;
        s.swapIq = (swap == 1);
        s.iqCentreHz = 7.05e6;
        CHECK(src.openWith(s));
        CHECK(src.start());
        CHECK_NEAR(src.sampleRateHz(), fs, 0.0);
        CHECK_NEAR(src.centerFrequencyHz(), 7.05e6, 0.0);
        // Nominal in I/Q mode: the external receiver's frequency is recorded.
        CHECK(src.setCenterFrequencyHz(7.1e6));
        CHECK_NEAR(src.centerFrequencyHz(), 7.1e6, 0.0);
        pushGenerated(*fake, 2, kFft + 1024, iq);
        const auto z = drain(src, kFft + 1024);
        CHECK(z.size() == kFft + 1024);
        if (z.size() < kFft) { continue; }
        const Peak pk = strongest(powerSpectrum(z), fs);
        std::printf("I/Q mode, swap %s: tone at %+.1f Hz from the centre\n", swap ? "on" : "off", pk.hz);
        CHECK_NEAR(pk.hz, swap ? -f : f, fs / kFft);
        // And the exact samples: left is I, right is Q (or the other way).
        const float i0 = 0.3f;  // cos(0)
        CHECK_NEAR(z[0].real(), swap ? 0.0f : i0, 1e-6);
        CHECK_NEAR(z[0].imag(), swap ? i0 : 0.0f, 1e-6);
    }
}

// --- 4. rates ------------------------------------------------------------------------

void testRates() {
    const SoundCardDevice card = stereoCard("Line In", "Windows WASAPI", 2, ratesUpTo192k());
    // REAL: a card rate whose half is not a whole number the pipeline takes
    // (8 kHz -> 4 kHz, 11.025 kHz -> 5512.5 Hz) is not offered.
    const auto real = cascade::source::soundCardRatesFor(card, SoundCardFormat::RealMono);
    std::vector<double> realHz;
    for (const auto& r : real) { realHz.push_back(r.hz); }
    CHECK((realHz == std::vector<double>{16000.0, 44100.0, 48000.0, 96000.0, 192000.0}));
    const auto iq = cascade::source::soundCardRatesFor(card, SoundCardFormat::IqStereo);
    CHECK(iq.size() == 7);
    // A one-channel card cannot do I/Q at all.
    const SoundCardDevice mono = stereoCard("Mic", "MME", 1, ratesUpTo192k(), 1);
    CHECK(cascade::source::soundCardRatesFor(mono, SoundCardFormat::IqStereo).empty());

    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {card, mono};
    SoundCardSource src = makeSource(fake);
    SoundCardSettings s;
    s.device = "Line In";
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = 190000.0;  // not offered: the nearest is 192 kHz
    CHECK(src.openWith(s));
    CHECK_NEAR(fake->lastRate, 192000.0, 0.0);
    CHECK(fake->lastExclusive);  // 192 kHz is an exclusive-only rate on this card
    CHECK(std::string(src.lastError()).find("192000") != std::string::npos);
    CHECK_NEAR(src.settings().cardRateHz, 192000.0, 0.0);
    CHECK_NEAR(src.sampleRateHz(), 96000.0, 0.0);
    CHECK((src.supportedSampleRatesHz() == std::vector<double>{8000.0, 22050.0, 24000.0, 48000.0, 96000.0}));
    double lo = 0.0, hi = 0.0;
    CHECK(src.frequencyRangeHz(lo, hi));
    CHECK_NEAR(lo, 0.0, 0.0);
    CHECK_NEAR(hi, 96000.0, 0.0);
    // The centre of a real-mode card cannot move.
    CHECK(!src.setCenterFrequencyHz(100000.0));
    CHECK(src.setCenterFrequencyHz(48000.0));
    // A new COMPLEX rate reopens the stream at the matching CARD rate, shared
    // mode this time.
    CHECK(src.setSampleRateHz(24000.0));
    CHECK_NEAR(fake->lastRate, 48000.0, 0.0);
    CHECK(!fake->lastExclusive);
    CHECK_NEAR(src.sampleRateHz(), 24000.0, 0.0);
    CHECK(fake->openCalls == 2);

    // I/Q on the one-channel card and the right channel of it are refused,
    // and nothing is opened.
    SoundCardSource m = makeSource(fake);
    SoundCardSettings ms;
    ms.device = "Mic";
    ms.hostApi = "MME";
    ms.format = SoundCardFormat::IqStereo;
    const int before = fake->openCalls;
    CHECK(!m.openWith(ms));
    ms.format = SoundCardFormat::RealMono;
    ms.channel = 1;
    CHECK(!m.openWith(ms));
    CHECK(fake->openCalls == before);
    ms.channel = 0;
    CHECK(m.openWith(ms));
    CHECK(fake->lastChannels == 1);
}

// --- 5. the device is found by name AND host API, or not at all ----------------------

void testMissingDevice() {
    auto fake = std::make_shared<FakeBackend>();
    // The same card, listed under MME only - the WASAPI entry the settings
    // name is not there (a different machine, or the card unplugged).
    fake->devices = {stereoCard("USB Audio CODEC", "MME", 0, ratesUpTo192k()),
                     stereoCard("Microphone (Realtek)", "Windows WASAPI", 1, ratesUpTo192k())};
    fake->devices[1].isDefault = true;
    SoundCardSource src = makeSource(fake);
    SoundCardSettings s;
    s.device = "USB Audio CODEC";
    s.hostApi = "Windows WASAPI";
    CHECK(!src.openWith(s));
    CHECK(fake->openCalls == 0);  // NOTHING opened in its place
    const std::string why = src.lastError();
    CHECK(why.find("not connected") != std::string::npos);
    CHECK(why.find("USB Audio CODEC") != std::string::npos);
    CHECK(!src.isOpen());
    // An empty name is "the default input" - the patch's unchosen node.
    SoundCardSettings d;
    CHECK(src.openWith(d));
    CHECK(fake->lastIndex == 1);
    // The pure lookup.
    CHECK(cascade::source::findSoundCard(fake->devices, "USB Audio CODEC", "MME") == 0);
    CHECK(cascade::source::findSoundCard(fake->devices, "USB Audio CODEC", "Windows WASAPI") == -1);
}

// --- 6. liveness ---------------------------------------------------------------------

void testLiveness() {
    // (a) the host API says the stream stopped: noticed within a poll or two.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        CHECK(src.openWith(s));
        CHECK(src.start());
        pushGenerated(*fake, 2, 4800, twoChannelTones(48000.0, 1000.0, 1000.0));
        (void)drain(src, 100000);
        CHECK(!src.faulted());
        fake->aliveFlag = false;  // the card is pulled out
        const auto t0 = Clock::now();
        std::vector<std::complex<float>> buf(512);
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(3)) {
            CHECK(src.read(buf.data(), buf.size()) == 0);
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("liveness: a stopped stream was noticed after %.0f ms\n", ms);
        CHECK(src.faulted());
        CHECK(ms < 1000.0);
        CHECK(std::string(src.lastError()).find("stopped") != std::string::npos);
        CHECK(src.deviceDead());
        CHECK(!src.start());  // a dead card is not restarted by the pipeline
    }
    // (b) the stream still claims to run but no samples arrive: the stall.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        CHECK(src.openWith(s));
        CHECK(src.start());
        const auto t0 = Clock::now();
        std::vector<std::complex<float>> buf(512);
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(5)) {
            (void)src.read(buf.data(), buf.size());
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("liveness: a silent stream was called dead after %.0f ms\n", ms);
        CHECK(src.faulted());
        CHECK(ms >= 1900.0 && ms < 3000.0);
        CHECK(std::string(src.lastError()).find("delivered nothing") != std::string::npos);
    }
    // (c) stop() gets an in-flight read out promptly, and is not a fault.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        CHECK(src.openWith(s));
        CHECK(src.start());
        src.stop();
        std::vector<std::complex<float>> buf(512);
        const auto t0 = Clock::now();
        CHECK(src.read(buf.data(), buf.size()) == 0);
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(50));
        CHECK(!src.faulted());
    }
}

// --- 7. the pipeline, end to end: the tester's own case -----------------------------

// SAQ at 17.2 kHz, on the left channel of a card at 192 kHz, with a little
// noise so the audio chain always has something to work on.
Generator saq(double fs) {
    auto rng = std::make_shared<std::mt19937>(12345u);
    return [=](float* out, std::size_t frames, std::uint64_t first, int ch) {
        std::normal_distribution<float> noise(0.0f, 0.001f);
        for (std::size_t i = 0; i < frames; ++i) {
            const double t = static_cast<double>(first + i) / fs;
            const float v = static_cast<float>(0.2 * std::cos(kTwoPi * 17200.0 * t)) + noise(*rng);
            for (int c = 0; c < ch; ++c) { out[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = v; }
        }
    };
}

// The audio tone's frequency by interpolated zero crossings, and the share
// of the audio's power within +/-50 Hz of it (a Goertzel scan).
struct Tone {
    double hz = 0.0;
    double purity = 0.0;
};

Tone measureTone(const std::vector<float>& a, double rate) {
    Tone t;
    // Zero crossings, upward, linearly interpolated.
    std::vector<double> ups;
    for (std::size_t i = 1; i < a.size(); ++i) {
        if (a[i - 1] < 0.0f && a[i] >= 0.0f) {
            const double frac = a[i - 1] / static_cast<double>(a[i - 1] - a[i]);
            ups.push_back(static_cast<double>(i - 1) + frac);
        }
    }
    if (ups.size() >= 3) {
        t.hz = rate * static_cast<double>(ups.size() - 1) / (ups.back() - ups.front());
    }
    const auto goertzel = [&](double f) {
        const double w = kTwoPi * f / rate;
        const double c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (const float v : a) {
            const double s0 = v + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    };
    double total = 0.0, near = 0.0;
    for (double f = 50.0; f < 4000.0; f += 10.0) {
        const double p = goertzel(f);
        total += p;
        if (std::fabs(f - 800.0) <= 50.0) { near += p; }
    }
    t.purity = total > 0.0 ? near / total : 0.0;
    return t;
}

struct EndToEnd {
    double spectrumPeakAirHz = 0.0;
    Tone tone;
};

EndToEnd runPipeline(double dialHz) {
    EndToEnd r;
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 4, ratesUpTo192k())};
    fake->gen = saq(192000.0);
    auto src = std::make_unique<SoundCardSource>(
        [fake] { return std::static_pointer_cast<SoundCardBackend>(fake); });
    SoundCardSettings s;
    s.device = "USB Audio CODEC";
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = 192000.0;
    CHECK(src->openWith(s));

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;  // the generator's rate, as in the app
    cfg.fftSize = 8192;
    cfg.audioEnabled = false;
    cascade::core::Pipeline p(cfg);
    // The application's order: install, then follow the source's rate.
    p.setSource(std::move(src));
    CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
    const double centre = p.activeSource().centerFrequencyHz();
    const double rate = p.inputRateHz();
    CHECK_NEAR(rate, 96000.0, 0.0);
    CHECK_NEAR(centre, 48000.0, 0.0);
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setVfoBandwidthHz(2400.0);
    p.setVfoOffsetHz(dialHz - centre);
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    cascade::core::SpectrumFrame frame;
    CHECK(p.getLatestFrame(frame));
    if (!frame.dbBins.empty()) {
        const std::size_t n = frame.dbBins.size();
        std::size_t at = 0;
        for (std::size_t k = 0; k < n; ++k) {
            if (frame.dbBins[k] > frame.dbBins[at]) { at = k; }
        }
        // fftshifted: bin n/2 is the centre.
        r.spectrumPeakAirHz =
            centre + (static_cast<double>(at) - static_cast<double>(n) / 2.0) * rate / static_cast<double>(n);
    }
    std::vector<float> audio(4096);
    CHECK(p.audioTap(audio.data(), audio.size()) == audio.size());
    r.tone = measureTone(audio, 48000.0);
    CHECK(!p.faulted());
    p.stop();
    return r;
}

void testPipelineSaq() {
    const EndToEnd usb = runPipeline(16400.0);
    std::printf("pipeline: spectrum peak at %.1f Hz on the air axis; USB at 16.4 kHz -> %.2f Hz "
                "audio, %.1f%% of the audio power within 50 Hz of 800 Hz\n",
                usb.spectrumPeakAirHz, usb.tone.hz, 100.0 * usb.tone.purity);
    CHECK_NEAR(usb.spectrumPeakAirHz, 17200.0, 96000.0 / 8192.0);
    CHECK_NEAR(usb.tone.hz, 800.0, 2.0);
    CHECK(usb.tone.purity > 0.9);
    // The same receiver 400 Hz ABOVE the tone must not hear it: 17.2 kHz is
    // then on the lower sideband. A mirrored spectrum would get this wrong.
    const EndToEnd above = runPipeline(17600.0);
    std::printf("pipeline: USB at 17.6 kHz (tone on the lower sideband) -> %.1f%% of the audio "
                "power within 50 Hz of 800 Hz\n",
                100.0 * above.tone.purity);
    CHECK(above.tone.purity < 0.2);
}

// A card pulled out while the pipeline streams: the pipeline's own fault
// state, with the source's reason, which is what the Source section shows.
void testPipelineFault() {
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 4, ratesUpTo192k())};
    fake->gen = saq(48000.0);
    auto src = std::make_unique<SoundCardSource>(
        [fake] { return std::static_pointer_cast<SoundCardBackend>(fake); });
    SoundCardSettings s;
    s.device = "USB Audio CODEC";
    s.hostApi = "Windows WASAPI";
    CHECK(src->openWith(s));
    cascade::core::Pipeline::Config cfg;
    cfg.audioEnabled = false;
    cascade::core::Pipeline p(cfg);
    p.setSource(std::move(src));
    CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!p.faulted());
    fake->stopFeeding();
    fake->aliveFlag = false;
    const auto t0 = Clock::now();
    while (!p.faulted() && Clock::now() - t0 < std::chrono::seconds(4)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(p.faulted());
    std::printf("pipeline: unplugged card -> \"%s\"\n", p.faultMessage().c_str());
    CHECK(p.faultMessage().find("USB Audio CODEC") != std::string::npos);
    p.stop();
    // Swapping the dead source out closes its stream off this thread; the
    // fake's close is still counted once that thread has run.
    p.setSource(nullptr);
    const auto t1 = Clock::now();
    while (fake->closeCalls.load() == 0 && Clock::now() - t1 < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->closeCalls.load() >= 1);
}

// --- 8. args, config, the Source section's rules -------------------------------------

void testArgsRoundTrip() {
    SoundCardSettings s;
    s.device = "Line In (2- USB Audio CODEC, rev=2|x%)";
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = 176400.0;
    s.format = SoundCardFormat::IqStereo;
    s.channel = 1;
    s.swapIq = true;
    s.iqCentreHz = 7050000.5;
    const std::string args = cascade::source::soundCardArgs(s);
    // No bare separator of the grammar survives inside a field.
    CHECK(args.find("CODEC,") == std::string::npos);
    CHECK(args.find("|") == std::string::npos);
    SoundCardSettings back;
    CHECK(cascade::source::parseSoundCardArgs(args, back));
    CHECK(back.device == s.device);
    CHECK(back.hostApi == s.hostApi);
    CHECK_NEAR(back.cardRateHz, s.cardRateHz, 0.0);
    CHECK(back.format == s.format);
    CHECK(back.channel == 1);
    CHECK(back.swapIq);
    CHECK_NEAR(back.iqCentreHz, s.iqCentreHz, 0.001);
    SoundCardSettings bad;
    CHECK(!cascade::source::parseSoundCardArgs("format=wideband", bad));
    CHECK(!cascade::source::parseSoundCardArgs("rate=fast", bad));
    CHECK(!cascade::source::parseSoundCardArgs("device=abc%2", bad));
    CHECK(!cascade::source::parseSoundCardArgs("channel=middle", bad));
    // DeviceSource::open takes the same string.
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard(s.device, s.hostApi, 9, {{176400.0, false}})};
    SoundCardSource src = makeSource(fake);
    CHECK(src.open(args));
    CHECK(fake->lastIndex == 9);
    CHECK(std::string(src.driverKey()) == "soundcard");
}

std::string tempPath(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path();
    return (dir / ("foxsdr_soundcard_" + std::string(tag) + "_" + std::to_string(TEST_GETPID()) + ".json"))
        .string();
}

void testConfigRoundTrip() {
    using cascade::core::AppConfig;
    using cascade::core::ConfigStore;
    AppConfig cfg;
    cfg.sourceKind = "soundcard";
    cfg.soundCard.device = "Line In (2- USB Audio CODEC)";
    cfg.soundCard.hostApi = "Windows WASAPI";
    cfg.soundCard.rateHz = 192000.0;
    cfg.soundCard.format = "iq";
    cfg.soundCard.channel = 1;
    cfg.soundCard.swapIq = true;
    cfg.soundCard.centreHz = 7050000.0;
    const std::string path = tempPath("cfg");
    std::string err;
    CHECK(ConfigStore::save(path, cfg, err));
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.sourceKind == "soundcard");
    CHECK(back.soundCard.device == cfg.soundCard.device);
    CHECK(back.soundCard.hostApi == cfg.soundCard.hostApi);
    CHECK_NEAR(back.soundCard.rateHz, 192000.0, 0.0);
    CHECK(back.soundCard.format == "iq");
    CHECK(back.soundCard.channel == 1);
    CHECK(back.soundCard.swapIq);
    CHECK_NEAR(back.soundCard.centreHz, 7050000.0, 0.0);

    // Hand-edited nonsense goes back to the defaults, field by field.
    std::string text = ConfigStore::serialize(cfg);
    const auto put = [&](const std::string& from, const std::string& to) {
        const std::size_t at = text.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) { text.replace(at, from.size(), to); }
    };
    put("\"format\": \"iq\"", "\"format\": \"wideband\"");
    put("\"channel\": 1", "\"channel\": 5");
    put("\"rateHz\": 192000.0", "\"rateHz\": 5000000.0");
    put("\"centreHz\": 7050000.0", "\"centreHz\": \"seven\"");
    CHECK(ConfigStore::writeFile(path, text, err));
    AppConfig odd;
    CHECK(ConfigStore::load(path, odd, err));
    CHECK(odd.sourceKind == "soundcard");
    CHECK(odd.soundCard.format == "real");
    CHECK(odd.soundCard.channel == 0);
    CHECK_NEAR(odd.soundCard.rateHz, 48000.0, 0.0);
    CHECK_NEAR(odd.soundCard.centreHz, 0.0, 0.0);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // The Source section's mapping both ways.
    const SoundCardSettings s = cascade::gui::soundCardFromConfig(cfg.soundCard);
    CHECK(s.format == SoundCardFormat::IqStereo);
    CHECK(s.swapIq && s.channel == 1);
    const AppConfig::SoundCard c2 = cascade::gui::soundCardToConfig(s);
    CHECK(c2.device == cfg.soundCard.device && c2.hostApi == cfg.soundCard.hostApi);
    CHECK(c2.format == "iq" && c2.channel == 1 && c2.swapIq);
    CHECK_NEAR(c2.rateHz, 192000.0, 0.0);
    CHECK_NEAR(c2.centreHz, 7050000.0, 0.0);
}

void testRememberedAndTune() {
    // A card that did not open at startup: the session runs on the generator,
    // and the save still names the sound card.
    const cascade::gui::RememberedSource keep =
        cascade::gui::rememberedSourceAfterFailedOpen("soundcard", "", "", "", 96000.0);
    CHECK(keep.valid());
    CHECK(keep.kind == "soundcard");
    const cascade::gui::SavedSource saved = cascade::gui::sourceToSave("siggen", "", "", "", 2e6, keep);
    CHECK(saved.kind == "soundcard");

    // Tuning a fixed centre moves the VFO instead. Real card at 192 kHz:
    // centre 48 kHz, span 0 - 96 kHz.
    const auto t = cascade::gui::tuneWithFixedCentre(17200.0, 0.0, 48000.0, 96000.0);
    CHECK(t.inside);
    CHECK_NEAR(t.wantAbsHz, 17200.0, 0.0);
    // The offset the caller was keeping is part of where it wanted the VFO.
    const auto k = cascade::gui::tuneWithFixedCentre(48000.0, -31600.0, 48000.0, 96000.0);
    CHECK(k.inside);
    CHECK_NEAR(k.wantAbsHz, 16400.0, 0.0);
    const auto out = cascade::gui::tuneWithFixedCentre(120000.0, 0.0, 48000.0, 96000.0);
    CHECK(!out.inside);
    CHECK_NEAR(out.loHz, 0.0, 0.0);
    CHECK_NEAR(out.hiHz, 96000.0, 0.0);
}

// The patch page: a radio's key names only the card; the Source section's
// settings apply when it is the same card; a SoapySDR scan has nothing to fear
// from a sound card.
void testPatchRules() {
    const std::string key = cascade::source::soundCardDeviceArgs("Line In (2- USB, rev|2)", "MME");
    CHECK(key.find('|') == std::string::npos);  // it sits inside a "driver|args" key
    SoundCardSettings section;
    section.device = "Line In (2- USB, rev|2)";
    section.hostApi = "MME";
    section.format = SoundCardFormat::IqStereo;
    section.swapIq = true;
    section.iqCentreHz = 7.05e6;
    section.cardRateHz = 96000.0;
    SoundCardSettings same;
    CHECK(cascade::source::parseSoundCardArgs(cascade::gui::soundCardArgsForPatch(key, section), same));
    CHECK(same.device == section.device && same.hostApi == "MME");
    CHECK(same.format == SoundCardFormat::IqStereo && same.swapIq);
    CHECK_NEAR(same.iqCentreHz, 7.05e6, 0.001);
    // Another card: plain real mono, left.
    SoundCardSettings other;
    CHECK(cascade::source::parseSoundCardArgs(
        cascade::gui::soundCardArgsForPatch(
            cascade::source::soundCardDeviceArgs("Line In (2- USB, rev|2)", "Windows WASAPI"), section),
        other));
    CHECK(other.hostApi == "Windows WASAPI");
    CHECK(other.format == SoundCardFormat::RealMono && other.channel == 0 && !other.swapIq);
    CHECK(!cascade::gui::scanMayProbe({"rtlsdr"}, "soundcard", key));
}

// --- 9. the real PortAudio backend, enumeration only --------------------------------

void testRealBackendEnumerates() {
    // On a worker, as the application does it.
    auto fut = std::async(std::launch::async, [] {
        auto b = cascade::source::makePortAudioSoundCardBackend();
        return b->listDevices();
    });
    const std::vector<SoundCardDevice> list = fut.get();
    std::printf("real backend: %zu input device(s)\n", list.size());
    for (const SoundCardDevice& d : list) {
        std::string rates;
        for (const SoundCardRate& r : d.rates) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s%.0f%s", rates.empty() ? "" : " ", r.hz, r.exclusive ? "x" : "");
            rates += buf;
        }
        std::printf("  [%d] %s | ch %d | default %.0f%s | %s\n", d.index,
                    cascade::source::soundCardDeviceLabel(d).c_str(), d.maxInputChannels,
                    d.defaultRateHz, d.isDefault ? " | DEFAULT" : "", rates.c_str());
        // Every listed rate is one the host API accepted; it must be a real one.
        for (const SoundCardRate& r : d.rates) { CHECK(r.hz >= 8000.0 && r.hz <= 384000.0); }
    }
}

}  // namespace

int main() {
    testRealToIqAxis();
    testRealModeChannel();
    testIqModeAndSwap();
    testRates();
    testMissingDevice();
    testLiveness();
    testPipelineSaq();
    testPipelineFault();
    testArgsRoundTrip();
    testConfigRoundTrip();
    testRememberedAndTune();
    testPatchRules();
    testRealBackendEnumerates();
    return testSummary("test_soundcard_source");
}
