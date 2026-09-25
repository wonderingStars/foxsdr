// Tests for the SOUND CARD source (source/soundcard_source.hpp), its real-to-
// complex conversion (dsp/real_to_iq.hpp) and the Source section's rules for
// it (gui/soundcard_panel.hpp, the remembered-source rule in tune_control.hpp,
// the config fields).
//
// NO SOUND CARD IS USED. The source is handed a FAKE backend that lists
// scripted devices and pushes scripted frames through exactly the callback
// path PortAudio drives, so ctest needs no audio hardware. The REAL PortAudio
// backend is also run against a SCRIPTED PortAudio (section 12): a Windows
// machine's worth of host APIs to prove only WASAPI is offered, and an abort
// that never returns to prove no lock a close holds is one a list, an open or
// a construction takes. The two sections that talk to the real PortAudio
// only initialise it and ENUMERATE (nothing is opened) and assert nothing
// about what they find beyond PortAudio's own init count.
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
#include <condition_variable>
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
#include "core/patch_devices.hpp"
#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "dsp/fft.hpp"
#include "dsp/real_to_iq.hpp"
#include "gui/device_scan_plan.hpp"
#include "gui/soundcard_panel.hpp"
#include "gui/tune_control.hpp"
#include "sink/pa_init.hpp"
#include "source/soundcard_portaudio.hpp"
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
        if (failOpen || std::find(refuseRatesHz.begin(), refuseRatesHz.end(), rateHz) != refuseRatesHz.end()) {
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

    bool alive() override {
        ++aliveCalls;
        return isOpen_ && aliveFlag.load();
    }

    std::atomic<int> aliveCalls{0};
    std::vector<double> refuseRatesHz;  // card rates open() refuses

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
    // centre 48 kHz, span 0 - 96 kHz, a 2.4 kHz USB filter.
    const auto t = cascade::gui::tuneWithFixedCentre(17200.0, 0.0, 48000.0, 96000.0, 2400.0);
    CHECK(t.inside);
    CHECK_NEAR(t.wantAbsHz, 17200.0, 0.0);
    // The offset the caller was keeping is part of where it wanted the VFO.
    const auto k = cascade::gui::tuneWithFixedCentre(48000.0, -31600.0, 48000.0, 96000.0, 2400.0);
    CHECK(k.inside);
    CHECK_NEAR(k.wantAbsHz, 16400.0, 0.0);
    const auto out = cascade::gui::tuneWithFixedCentre(120000.0, 0.0, 48000.0, 96000.0, 2400.0);
    CHECK(!out.inside);
    CHECK(!out.tooWide);
    // The sentence gives the VFO's REACH with this filter: the span less half
    // the filter at each end.
    CHECK_NEAR(out.loHz, 1200.0, 0.0);
    CHECK_NEAR(out.hiHz, 94800.0, 0.0);
}

// Item 5 of the review: "inside" and the VFO's own clamp must be ONE rule. A
// tune within half a filter of the span's edge used to be reported inside and
// then moved by setVfoToAbsoluteHz's clamp - "never clamped" said the header.
void testFixedCentreEdge() {
    using cascade::gui::tuneWithFixedCentre;
    using cascade::gui::vfoOffsetInsideSpan;
    const double c = 48000.0, rate = 96000.0, bw = 2400.0;
    // The very edge of the reach is inside, and lands exactly as asked.
    const auto edge = tuneWithFixedCentre(94800.0, 0.0, c, rate, bw);
    CHECK(edge.inside);
    CHECK_NEAR(vfoOffsetInsideSpan(edge.wantAbsHz - c, rate, bw), edge.wantAbsHz - c, 0.0);
    // Half a hertz past it is refused - and so is 95 kHz, INSIDE the span but
    // within half the filter of its edge (the reviewer's case).
    CHECK(!tuneWithFixedCentre(94800.5, 0.0, c, rate, bw).inside);
    CHECK(!tuneWithFixedCentre(95000.0, 0.0, c, rate, bw).inside);
    CHECK(!tuneWithFixedCentre(1000.0, 0.0, c, rate, bw).inside);
    CHECK(tuneWithFixedCentre(1200.0, 0.0, c, rate, bw).inside);
    // THE PROPERTY, swept across and beyond the span in 100 Hz steps for three
    // filters: a tune is inside exactly when the VFO's clamp leaves it alone.
    int disagreements = 0;
    int insideCount = 0;
    for (const double w : {2400.0, 12000.0, 50000.0}) {
        for (double want = -10000.0; want <= 110000.0; want += 100.0) {
            const auto r = tuneWithFixedCentre(want, 0.0, c, rate, w);
            const double off = want - c;
            const bool unclamped = vfoOffsetInsideSpan(off, rate, w) == off;
            if (r.inside != unclamped) { ++disagreements; }
            if (r.inside) { ++insideCount; }
        }
    }
    std::printf("fixed centre: %d tunes inside, %d disagreements with the VFO clamp\n", insideCount,
                disagreements);
    CHECK(disagreements == 0);
    CHECK(insideCount > 1000);  // the sweep really did cover the reach
    // A filter wider than the span leaves no reach: every tune is refused,
    // with that reason, and never lands half outside.
    const auto wide = tuneWithFixedCentre(48000.0, 0.0, c, rate, 150000.0);
    CHECK(wide.tooWide);
    CHECK(!wide.inside);
    CHECK(tuneWithFixedCentre(48000.0, 0.0, c, rate, 96000.0).tooWide);  // exactly the span
    CHECK(!tuneWithFixedCentre(48000.0, 0.0, c, rate, 95999.0).tooWide);
}

// Item 4 of the review: WFM's 150 kHz filter on a 96 kHz card left the VFO
// wherever it was - half outside what the card delivers. It is centred.
void testVfoInsideSpan() {
    using cascade::gui::vfoOffsetInsideSpan;
    CHECK_NEAR(vfoOffsetInsideSpan(30000.0, 96000.0, 150000.0), 0.0, 0.0);   // wider: centred
    CHECK_NEAR(vfoOffsetInsideSpan(-30000.0, 96000.0, 96000.0), 0.0, 0.0);   // exactly the span
    CHECK_NEAR(vfoOffsetInsideSpan(60000.0, 96000.0, 2400.0), 46800.0, 0.0);  // brought inside
    CHECK_NEAR(vfoOffsetInsideSpan(-60000.0, 96000.0, 2400.0), -46800.0, 0.0);
    CHECK_NEAR(vfoOffsetInsideSpan(-31600.0, 96000.0, 2400.0), -31600.0, 0.0);  // left alone
    CHECK_NEAR(vfoOffsetInsideSpan(46800.0, 96000.0, 2400.0), 46800.0, 0.0);    // on the edge
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

// --- 10. the review's items: the capture block, start, rates, the alive poll ----------

// R1/R4: the realtime callback writes WHOLE frames and counts a block that did
// not fit. A 4-frame (8-float) ring offered 5 stereo frames keeps the first 4
// exactly and counts one overrun.
void testWholeFramesAndOverruns() {
    SoundCardSource::Capture cap(8);
    cap.channels.store(2);
    const float block[10] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5};
    SoundCardSource::pushFrames(&cap, block, 5);
    CHECK(cap.ring.size() == 8);
    CHECK(cap.overruns.load() == 1);
    float got[8] = {};
    CHECK(cap.ring.read(got, 8) == 8);
    bool same = true;
    for (int i = 0; i < 8; ++i) { same = same && got[i] == block[i]; }
    CHECK(same);
    // A block that fits is not an overrun.
    SoundCardSource::pushFrames(&cap, block, 3);
    CHECK(cap.overruns.load() == 1);
    CHECK(cap.ring.size() == 6);
    // Two free floats in a stereo ring are ONE frame, not two samples of two.
    SoundCardSource::pushFrames(&cap, block, 2);
    CHECK(cap.ring.size() == 8);
    CHECK(cap.overruns.load() == 2);
}

// A stereo I/Q generator whose I sample is a constant level, so the first
// sample read says which push it came from.
Generator constantIq(float level) {
    return [=](float* out, std::size_t frames, std::uint64_t /*first*/, int ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            out[i * static_cast<std::size_t>(ch)] = level;
            out[i * static_cast<std::size_t>(ch) + 1] = 0.0f;
        }
    };
}

SoundCardSettings iqSettings(const std::string& name, double rate) {
    SoundCardSettings s;
    s.device = name;
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = rate;
    s.format = SoundCardFormat::IqStereo;
    return s;
}

// R3: what the card captured while the pipeline was stopped is stale, and
// start() throws it away.
void testStartDrainsStale() {
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    SoundCardSource src = makeSource(fake);
    CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
    pushGenerated(*fake, 2, 3000, constantIq(0.25f));  // captured before start: stale
    CHECK(src.start());
    pushGenerated(*fake, 2, 1000, constantIq(0.75f));  // after start: fresh
    std::vector<std::complex<float>> buf(4096);
    const std::size_t got = src.read(buf.data(), buf.size());
    CHECK(got == 1000);
    CHECK_NEAR(got != 0 ? buf[0].real() : 0.0f, 0.75f, 0.0);
}

// R6 and item 7: a new rate reopens the card and it is still RUNNING; a rate
// the card refuses puts it back as it was and says so; and when even that
// fails the card is reported closed, never as running.
void testRateChange() {
    // (a) accepted: running again, samples flow.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        CHECK(src.setSampleRateHz(96000.0));
        CHECK(src.running());
        CHECK_NEAR(fake->lastRate, 96000.0, 0.0);
        pushGenerated(*fake, 2, 500, constantIq(0.5f));
        std::vector<std::complex<float>> buf(1024);
        CHECK(src.read(buf.data(), buf.size()) == 500);
    }
    // (b) refused: back at 48 kHz, running, and the reason on the line.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        fake->refuseRatesHz = {96000.0};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        CHECK(!src.setSampleRateHz(96000.0));
        CHECK(src.isOpen());
        CHECK(src.running());
        CHECK_NEAR(src.sampleRateHz(), 48000.0, 0.0);
        CHECK_NEAR(fake->lastRate, 48000.0, 0.0);
        const std::string why = src.lastError();
        std::printf("rate change refused: \"%s\"\n", why.c_str());
        CHECK(why.find("fake refused") != std::string::npos);
        CHECK(why.find("still running at 48000 Hz") != std::string::npos);
        pushGenerated(*fake, 2, 500, constantIq(0.5f));
        std::vector<std::complex<float>> buf(1024);
        CHECK(src.read(buf.data(), buf.size()) == 500);
    }
    // (c) refused, and the old rate will not reopen either: CLOSED, and start()
    // refuses with both reasons.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        fake->failOpen = true;
        CHECK(!src.setSampleRateHz(96000.0));
        CHECK(!src.isOpen());
        CHECK(!src.running());
        CHECK(!src.start());
        const std::string why = src.lastError();
        std::printf("rate change and restore both refused: \"%s\"\n", why.c_str());
        CHECK(why.find("could not be reopened at 48000 Hz") != std::string::npos);
    }
}

// R8: while it waits for samples, read() asks the backend alive() every
// kAlivePollMs - not on every 2 ms turn of its wait.
void testAlivePollInterval() {
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    SoundCardSource src = makeSource(fake);
    CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
    CHECK(src.start());
    const int before = fake->aliveCalls.load();
    std::vector<std::complex<float>> buf(512);
    const auto t0 = Clock::now();
    while (Clock::now() - t0 < std::chrono::milliseconds(1000)) { (void)src.read(buf.data(), buf.size()); }
    const int calls = fake->aliveCalls.load() - before;
    std::printf("alive poll: %d calls in 1 s of waiting (every %lld ms)\n", calls,
                static_cast<long long>(SoundCardSource::kAlivePollMs.count()));
    CHECK(!src.faulted());
    CHECK(calls >= 2 && calls <= 5);
}

// R7: the config's rate bounds, the LOWER one too.
void testConfigRateBounds() {
    using cascade::core::AppConfig;
    using cascade::core::ConfigStore;
    const std::string path = tempPath("bounds");
    for (const double rate : {4000.0, 7999.0, 8000.0, 768000.0, 768001.0}) {
        AppConfig cfg;
        cfg.soundCard.rateHz = rate;
        std::string err;
        CHECK(ConfigStore::save(path, cfg, err));
        AppConfig back;
        CHECK(ConfigStore::load(path, back, err));
        const bool inRange = rate >= 8000.0 && rate <= 768000.0;
        CHECK_NEAR(back.soundCard.rateHz, inRange ? rate : 48000.0, 0.0);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Item 3: every message about a card that is missing or has stopped gives the
// same, true, advice - restart FoxSDR - and none of them "press Open".
void testRecoveryAdvice() {
    std::vector<std::string> lines;
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("Mic", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(!src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        lines.push_back(src.lastError());
    }
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        fake->aliveFlag = false;
        std::vector<std::complex<float>> buf(512);
        const auto t0 = Clock::now();
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(3)) { (void)src.read(buf.data(), buf.size()); }
        lines.push_back(src.lastError());
    }
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        std::vector<std::complex<float>> buf(512);
        const auto t0 = Clock::now();
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(4)) { (void)src.read(buf.data(), buf.size()); }
        lines.push_back(src.lastError());
    }
    CHECK(lines.size() == 3);
    for (const std::string& l : lines) {
        std::printf("advice: \"%s\"\n", l.c_str());
        CHECK(l.find("restart FoxSDR") != std::string::npos);
        CHECK(l.find("press Open") == std::string::npos);
    }
}

// Items 6 and 7: the patch page takes the receiver's sound card as it takes a
// radio, under the key its device list gives that card; the centre box moves
// only a card RUNNING in I/Q mode.
void testPatchLoanAndCentreBox() {
    using cascade::gui::receiverSourceForPatch;
    SoundCardSettings card;
    card.device = "Line In (2- USB, rev|2)";
    card.hostApi = "Windows WASAPI";
    card.format = SoundCardFormat::IqStereo;
    const auto l = receiverSourceForPatch(true, "soundcard", false, false, "", false, card);
    CHECK(l.take);
    CHECK(l.kind == "soundcard");
    // The same key the patch's device list offers for that card, so a patch
    // radio already on it is on the lent card and not a second stream.
    const std::string listed = cascade::core::patch::makeDeviceKey(
        "soundcard", cascade::source::soundCardDeviceArgs(card.device, card.hostApi));
    CHECK(cascade::core::patch::makeDeviceKey(l.kind, l.args) == listed);
    // ...and it opens there as the Source section has it set up.
    SoundCardSettings opened;
    CHECK(cascade::source::parseSoundCardArgs(cascade::gui::soundCardArgsForPatch(l.args, card), opened));
    CHECK(opened.format == SoundCardFormat::IqStereo && opened.device == card.device);
    // Nothing while the patch is stopped, while an open is resolving, or from
    // a file or the generator; a radio as before.
    CHECK(!receiverSourceForPatch(false, "soundcard", false, false, "", false, card).take);
    CHECK(!receiverSourceForPatch(true, "soundcard", false, false, "", true, card).take);
    CHECK(!receiverSourceForPatch(true, "soundcard", false, true, "", false, card).take);
    CHECK(!receiverSourceForPatch(true, "siggen", false, false, "", false, card).take);
    CHECK(!receiverSourceForPatch(true, "file", false, false, "", false, card).take);
    const auto r = receiverSourceForPatch(true, "rtlsdr", true, false, "serial=1", false, card);
    CHECK(r.take && r.kind == "rtlsdr" && r.args == "serial=1");
    CHECK(!receiverSourceForPatch(true, "rtlsdr", true, true, "serial=1", false, card).take);

    using cascade::gui::soundCardCentreAppliesLive;
    CHECK(soundCardCentreAppliesLive(true, false, SoundCardFormat::IqStereo));
    CHECK(!soundCardCentreAppliesLive(true, false, SoundCardFormat::RealMono));  // the review's case
    CHECK(!soundCardCentreAppliesLive(false, false, SoundCardFormat::IqStereo));
    CHECK(!soundCardCentreAppliesLive(true, true, SoundCardFormat::IqStereo));
}

// --- 11. a close that never returns ----------------------------------------------------

// Runs `f` on a thread of its own and says whether it finished within `bound`.
// The future is kept (a std::async future's destructor waits), so a step that
// hung is joined only after the test has released whatever hung it.
struct Bounded {
    std::future<void> fut;
    bool finished = false;
    double ms = 0.0;
};

Bounded runBounded(std::function<void()> f, std::chrono::milliseconds bound) {
    Bounded b;
    const auto t0 = Clock::now();
    b.fut = std::async(std::launch::async, std::move(f));
    b.finished = b.fut.wait_for(bound) == std::future_status::ready;
    b.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return b;
}

// A BACKEND WHOSE CLOSE NEVER RETURNS, with the lock a real backend has:
// open, list and close all take this backend's own mutex, and the close holds
// it while it hangs - which is what 6b5aead's PortAudio backend did (with a
// lock shared by every backend). A source that closes on the caller's thread,
// or opens again on the backend its close took, waits on that lock for ever.
class HangingBackend final : public SoundCardBackend {
public:
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool released = false;
        std::atomic<int> hung{0};    // closes that are hanging now
        std::atomic<int> closed{0};  // closes that have returned
    };

    HangingBackend(std::shared_ptr<Gate> gate, bool hangOnClose, std::vector<SoundCardDevice> devices)
        : gate_(std::move(gate)), hang_(hangOnClose), devices_(std::move(devices)) {}

    std::vector<SoundCardDevice> listDevices() override {
        std::lock_guard<std::mutex> lk(m_);
        return devices_;
    }
    bool open(const SoundCardDevice&, int, double, bool, PushFn push, void* user, std::string& error) override {
        std::lock_guard<std::mutex> lk(m_);
        if (open_) {
            error = "already open";
            return false;
        }
        push_ = push;
        user_ = user;
        open_ = true;
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lk(m_);
        if (!open_) { return; }
        if (hang_) {
            ++gate_->hung;
            std::unique_lock<std::mutex> g(gate_->m);
            gate_->cv.wait(g, [this] { return gate_->released; });
            --gate_->hung;
        }
        open_ = false;
        ++gate_->closed;
    }
    bool alive() override {
        std::unique_lock<std::mutex> lk(m_, std::try_to_lock);
        return !lk.owns_lock() || open_;
    }
    void push(const float* x, std::size_t frames) {
        if (push_ != nullptr) { push_(user_, x, frames); }
    }

private:
    std::shared_ptr<Gate> gate_;
    bool hang_;
    std::vector<SoundCardDevice> devices_;
    std::mutex m_;
    bool open_ = false;
    PushFn push_ = nullptr;
    void* user_ = nullptr;
};

void releaseGate(HangingBackend::Gate& g) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.released = true;
    }
    g.cv.notify_all();
}

// Item 1, at the source: a switch, a destroy and a new open all finish within
// a bound while a close hangs, and the hung close is left on its own thread.
void testHungCloseAtTheSource() {
    const std::vector<SoundCardDevice> devices = {
        stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    const auto bound = SoundCardSource::kCloseWaitMs + std::chrono::milliseconds(700);

    // (a) A HEALTHY card whose close hangs: destroying the source costs at
    // most kCloseWaitMs, the close is counted as left behind, and the SAME
    // source object opens again at once on a fresh backend.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::shared_ptr<HangingBackend>> made;
        std::mutex madeM;
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            std::lock_guard<std::mutex> lk(madeM);
            // Only the FIRST backend hangs on close.
            made.push_back(std::make_shared<HangingBackend>(gate, made.empty(), devices));
            return made.back();
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src->start());
        const std::uint64_t abandonedBefore = SoundCardSource::abandonedCloses();
        // The source is reopened (a new rate is the same path: close, open).
        Bounded reopen = runBounded([&] { (void)src->openWith(iqSettings("USB Audio CODEC", 96000.0)); }, bound);
        std::printf("hung close: a reopen of the same source took %.0f ms (%s)\n", reopen.ms,
                    reopen.finished ? "finished" : "STILL WAITING");
        CHECK(reopen.finished);
        CHECK(gate->hung.load() == 1);
        CHECK(SoundCardSource::abandonedCloses() == abandonedBefore + 1);
        if (!reopen.finished) {
            // RED: the reopen is stuck behind the hung close. Let it go before
            // anything else touches the source.
            releaseGate(*gate);
            reopen.fut.get();
        }
        if (reopen.finished) {
            CHECK(src->isOpen());
            CHECK(src->start());
            {
                std::lock_guard<std::mutex> lk(madeM);
                CHECK(made.size() == 2);  // a fresh backend, not the one the close took
                const float frame[2] = {0.5f, 0.0f};
                if (made.size() == 2) { made[1]->push(frame, 1); }
            }
            std::vector<std::complex<float>> buf(16);
            CHECK(src->read(buf.data(), buf.size()) == 1);
        }
        // Destroying it (a source switch, the exit) is prompt too: its own
        // backend's close does not hang.
        Bounded destroy = runBounded([&] { src.reset(); }, bound);
        std::printf("hung close: destroying the reopened source took %.0f ms\n", destroy.ms);
        CHECK(destroy.finished);
        releaseGate(*gate);
        if (reopen.fut.valid()) { reopen.fut.get(); }
        if (destroy.fut.valid()) { destroy.fut.get(); }
        const auto t0 = Clock::now();
        while (gate->closed.load() < 2 && Clock::now() - t0 < std::chrono::seconds(3)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(gate->closed.load() == 2);  // the hung close finished once released
    }

    // (b) The destroy itself, with the close hanging: bounded by kCloseWaitMs.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            return std::make_shared<HangingBackend>(gate, true, devices);
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        Bounded destroy = runBounded([&] { src.reset(); }, bound);
        std::printf("hung close: destroying a healthy source took %.0f ms (budget %lld ms)\n", destroy.ms,
                    static_cast<long long>(SoundCardSource::kCloseWaitMs.count()));
        CHECK(destroy.finished);
        CHECK(destroy.ms >= static_cast<double>(SoundCardSource::kCloseWaitMs.count()) - 20.0);
        releaseGate(*gate);
        if (destroy.fut.valid()) { destroy.fut.get(); }
    }

    // (c) A card that has FAILED is not waited for at all - through the
    // pipeline, as the application swaps it out.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::shared_ptr<HangingBackend> dead;
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            dead = std::make_shared<HangingBackend>(gate, true, devices);
            return dead;
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        cascade::core::Pipeline::Config cfg;
        cfg.audioEnabled = false;
        cascade::core::Pipeline p(cfg);
        p.setSource(std::move(src));
        CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
        p.start();
        // Nothing arrives: the stall latches the card dead.
        const auto t0 = Clock::now();
        while (!p.faulted() && Clock::now() - t0 < std::chrono::seconds(4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(p.faulted());
        p.stop();
        Bounded swap = runBounded([&] { p.setSource(nullptr); }, bound);
        std::printf("hung close: swapping out a dead card took %.0f ms\n", swap.ms);
        CHECK(swap.finished);
        CHECK(swap.ms < 300.0);
        releaseGate(*gate);
        if (swap.fut.valid()) { swap.fut.get(); }
    }

    // (d) The teardown switch: once the application's exit has begun no
    // close is waited for, healthy or not.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            return std::make_shared<HangingBackend>(gate, true, devices);
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        SoundCardSource::setCloseWaitEnabled(false);
        Bounded destroy = runBounded([&] { src.reset(); }, bound);
        SoundCardSource::setCloseWaitEnabled(true);
        std::printf("hung close: destroying at exit took %.0f ms\n", destroy.ms);
        CHECK(destroy.finished);
        CHECK(destroy.ms < 300.0);
        releaseGate(*gate);
        if (destroy.fut.valid()) { destroy.fut.get(); }
    }
}

// --- 12. the REAL PortAudio backend, over a scripted PortAudio ------------------------

namespace fakepa {

std::vector<PaHostApiInfo> apis;
std::vector<PaDeviceInfo> devs;
std::vector<std::string> names;  // storage behind devs[i].name
PaDeviceIndex defaultIn = paNoDevice;
std::atomic<int> inits{0};
std::atomic<int> openCalls{0};
std::atomic<int> closeCalls{0};
std::atomic<int> listCalls{0};

struct Stream {
    PaStreamCallback* cb = nullptr;
    void* user = nullptr;
    std::atomic<bool> active{false};
};
constexpr int kStreams = 32;
Stream streams[kStreams];
int streamIds[kStreams];
std::atomic<int> nextStream{0};

// The one stream whose abort never returns until released.
std::atomic<void*> hangOn{nullptr};
std::mutex gateM;
std::condition_variable gateCv;
bool released = false;
std::atomic<int> hung{0};

int indexOf(PaStream* s) { return static_cast<int>(static_cast<int*>(s) - streamIds); }

PaError initialize() {
    ++inits;
    return paNoError;
}
PaError terminate() {
    --inits;
    return paNoError;
}
PaDeviceIndex getDeviceCount() {
    ++listCalls;
    return static_cast<PaDeviceIndex>(devs.size());
}
PaDeviceIndex getDefaultInputDevice() { return defaultIn; }
const PaDeviceInfo* getDeviceInfo(PaDeviceIndex i) {
    return (i >= 0 && i < static_cast<PaDeviceIndex>(devs.size())) ? &devs[static_cast<std::size_t>(i)] : nullptr;
}
const PaHostApiInfo* getHostApiInfo(PaHostApiIndex h) {
    return (h >= 0 && h < static_cast<PaHostApiIndex>(apis.size())) ? &apis[static_cast<std::size_t>(h)] : nullptr;
}
PaError isFormatSupported(const PaStreamParameters* in, const PaStreamParameters*, double rate) {
    // Shared mode: 48 kHz. Exclusive (a WASAPI stream-info block): 192 kHz.
    const bool excl = in != nullptr && in->hostApiSpecificStreamInfo != nullptr;
    return (excl ? rate == 192000.0 : rate == 48000.0) ? paFormatIsSupported : paInvalidSampleRate;
}
PaError openStream(PaStream** s, const PaStreamParameters*, const PaStreamParameters*, double, unsigned long,
                   PaStreamFlags, PaStreamCallback* cb, void* user) {
    ++openCalls;
    const int k = nextStream++ % kStreams;
    streams[k].cb = cb;
    streams[k].user = user;
    streams[k].active = true;
    *s = &streamIds[k];
    return paNoError;
}
PaError startStream(PaStream*) { return paNoError; }
PaError abortStream(PaStream* s) {
    if (s == hangOn.load()) {
        ++hung;
        std::unique_lock<std::mutex> lk(gateM);
        gateCv.wait(lk, [] { return released; });
        --hung;
    }
    streams[indexOf(s)].active = false;
    return paNoError;
}
PaError closeStream(PaStream*) {
    ++closeCalls;
    return paNoError;
}
PaError isStreamActive(PaStream* s) { return streams[indexOf(s)].active.load() ? 1 : 0; }
const char* getErrorText(PaError) { return "scripted PortAudio error"; }

const cascade::source::SoundCardPaApi kApi = {
    &initialize,      &terminate,  &getDeviceCount, &getDefaultInputDevice, &getDeviceInfo,
    &getHostApiInfo,  &isFormatSupported, &openStream, &startStream, &abortStream,
    &closeStream,     &isStreamActive, &getErrorText,
};

void addApi(PaHostApiTypeId type, const char* name) {
    PaHostApiInfo a{};
    a.structVersion = 1;
    a.type = type;
    a.name = name;
    a.defaultInputDevice = paNoDevice;
    a.defaultOutputDevice = paNoDevice;
    apis.push_back(a);
}

void addDevice(const char* name, PaHostApiIndex api, int inputs, bool apiDefault = false) {
    PaDeviceInfo d{};
    d.structVersion = 2;
    d.hostApi = api;
    d.maxInputChannels = inputs;
    d.maxOutputChannels = inputs == 0 ? 2 : 0;
    d.defaultHighInputLatency = 0.1;
    d.defaultSampleRate = 48000.0;
    names.emplace_back(name);
    devs.push_back(d);
    if (apiDefault) { apis[static_cast<std::size_t>(api)].defaultInputDevice = static_cast<PaDeviceIndex>(devs.size() - 1); }
}

// Names are pointed at AFTER every push_back, when neither vector moves again.
void seal() {
    for (std::size_t i = 0; i < devs.size(); ++i) { devs[i].name = names[i].c_str(); }
    for (auto& a : apis) { a.deviceCount = 0; }
    for (const auto& d : devs) { ++apis[static_cast<std::size_t>(d.hostApi)].deviceCount; }
}

// What a Windows machine with one USB codec lists, under every host API
// PortAudio has - plus an ALSA entry, so the rule is seen to keep Linux's.
void scriptWindowsMachine() {
    apis.clear();
    devs.clear();
    names.clear();
    addApi(paMME, "MME");
    addApi(paDirectSound, "Windows DirectSound");
    addApi(paWASAPI, "Windows WASAPI");
    addApi(paWDMKS, "Windows WDM-KS");
    addApi(paALSA, "ALSA");
    addDevice("Microsoft Sound Mapper - Input", 0, 2, true);   // 0
    addDevice("USB Audio CODEC", 0, 2);                        // 1
    addDevice("Primary Sound Capture Driver", 1, 2, true);     // 2
    addDevice("Microphone (USB Audio CODEC)", 1, 2);           // 3
    addDevice("Microphone (USB Audio CODEC)", 2, 2, true);     // 4 <- WASAPI
    addDevice("Line (USB Audio CODEC)", 3, 2);                 // 5
    addDevice("hw:CARD=CODEC,DEV=0", 4, 2);                    // 6 <- ALSA
    addDevice("Speakers (USB Audio CODEC)", 2, 0);             // 7 output only
    seal();
    // PortAudio's own default input on Windows is MME's - the mapper.
    defaultIn = 0;
}

}  // namespace fakepa

// Item 2: only host APIs whose inputs keep their identity are offered, and a
// card saved from an MME entry is NOT FOUND - never substituted.
void testHostApiFilter() {
    fakepa::scriptWindowsMachine();
    auto b = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    const std::vector<SoundCardDevice> list = b->listDevices();
    for (const SoundCardDevice& d : list) {
        std::printf("scripted machine lists: [%d] %s\n", d.index, cascade::source::soundCardDeviceLabel(d).c_str());
    }
    CHECK(list.size() == 2);
    const bool wasapiOnly = list.size() == 2 && list[0].index == 4 && list[0].hostApi == "Windows WASAPI" &&
                            list[1].index == 6 && list[1].hostApi == "ALSA";
    CHECK(wasapiOnly);
    for (const SoundCardDevice& d : list) {
        CHECK(d.name.find("Mapper") == std::string::npos);
        CHECK(d.name.find("Primary Sound") == std::string::npos);
        CHECK(d.hostApi != "MME" && d.hostApi != "Windows DirectSound" && d.hostApi != "Windows WDM-KS");
    }
    // The WASAPI entry's rates: 48 kHz shared, 192 kHz exclusive (Windows).
    if (!list.empty()) {
        CHECK(!list[0].rates.empty() && list[0].rates[0].hz == 48000.0 && !list[0].rates[0].exclusive);
    }
    // PortAudio's own default is the MME mapper, which is not offered; the
    // default row is WASAPI's own default input instead.
    CHECK(cascade::source::findSoundCard(list, "", "") == 0);

    // A card saved by an earlier build from its MME entry: not connected, and
    // nothing at all opened in its place.
    const int opensBefore = fakepa::openCalls.load();
    SoundCardSource src([] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); });
    SoundCardSettings mme;
    mme.device = "USB Audio CODEC";
    mme.hostApi = "MME";
    CHECK(!src.openWith(mme));
    CHECK(fakepa::openCalls.load() == opensBefore);
    CHECK(std::string(src.lastError()).find("not connected") != std::string::npos);
    SoundCardSettings mapper;
    mapper.device = "Microsoft Sound Mapper - Input";
    mapper.hostApi = "MME";
    CHECK(!src.openWith(mapper));
    CHECK(fakepa::openCalls.load() == opensBefore);
    // ...and the backend itself refuses an MME entry handed to it directly,
    // whatever list it came from.
    SoundCardDevice stale;
    stale.index = 1;
    stale.name = "USB Audio CODEC";
    stale.hostApi = "MME";
    stale.maxInputChannels = 2;
    std::string err;
    auto b2 = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    CHECK(!b2->open(stale, 2, 48000.0, false, &SoundCardSource::pushFrames, nullptr, err));
    CHECK(fakepa::openCalls.load() == opensBefore);
    // The pure rule.
    CHECK(!cascade::source::soundCardHostApiListed(paMME));
    CHECK(!cascade::source::soundCardHostApiListed(paDirectSound));
    CHECK(cascade::source::soundCardHostApiListed(paWASAPI));
    CHECK(cascade::source::soundCardHostApiListed(paALSA));
}

// Item 1, in the REAL backend: while one card's abort hangs inside the host
// API, another backend enumerates, a new source opens and streams, and the
// hung source's destroy is bounded - so no lock the close holds is one that
// construction, listing or opening takes. (6b5aead's backend took ONE lock for
// all of them, and its close held it through Pa_AbortStream.)
void testRealBackendHungAbort() {
    fakepa::scriptWindowsMachine();
    const auto bound = SoundCardSource::kCloseWaitMs + std::chrono::milliseconds(700);
    const auto factory = [] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); };
    const int initsBefore = fakepa::inits.load();

    // A backend made on this thread calls nothing.
    {
        auto idle = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
        CHECK(fakepa::inits.load() == initsBefore);
    }

    auto a = std::make_unique<SoundCardSource>(factory);
    CHECK(a->openWith(iqSettings("Microphone (USB Audio CODEC)", 48000.0)));
    CHECK(a->start());
    const int aStream = (fakepa::nextStream.load() - 1) % fakepa::kStreams;
    fakepa::hangOn = &fakepa::streamIds[aStream];

    // The source is destroyed (a switch, the exit) and its abort hangs.
    Bounded destroyA = runBounded([&] { a.reset(); }, bound);
    std::printf("real backend: destroying a source whose abort hangs took %.0f ms\n", destroyA.ms);
    CHECK(destroyA.finished);
    CHECK(fakepa::hung.load() == 1);

    // With that abort still hung: another backend enumerates...
    Bounded list = runBounded(
        [&] {
            auto b = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
            CHECK(b->listDevices().size() == 2);
        },
        std::chrono::milliseconds(700));
    std::printf("real backend: an enumeration beside the hung abort took %.0f ms (%s)\n", list.ms,
                list.finished ? "finished" : "STILL WAITING");
    CHECK(list.finished);

    // ...a new source is made, opened and streams...
    std::unique_ptr<SoundCardSource> c;
    Bounded openC = runBounded(
        [&] {
            c = std::make_unique<SoundCardSource>(factory);
            CHECK(c->openWith(iqSettings("Microphone (USB Audio CODEC)", 48000.0)));
            CHECK(c->start());
        },
        std::chrono::milliseconds(700));
    std::printf("real backend: a new open beside the hung abort took %.0f ms (%s)\n", openC.ms,
                openC.finished ? "finished" : "STILL WAITING");
    CHECK(openC.finished);
    if (openC.finished && c && c->isOpen()) {
        const int k = (fakepa::nextStream.load() - 1) % fakepa::kStreams;
        const float frame[2] = {0.5f, 0.0f};
        fakepa::streams[k].cb(frame, nullptr, 1, nullptr, 0, fakepa::streams[k].user);
        std::vector<std::complex<float>> buf(16);
        CHECK(c->read(buf.data(), buf.size()) == 1);
        // ...and a switch away from it through a pipeline is prompt.
        cascade::core::Pipeline::Config cfg;
        cfg.audioEnabled = false;
        cascade::core::Pipeline p(cfg);
        p.setSource(std::move(c));
        Bounded swap = runBounded([&] { p.setSource(nullptr); }, bound);
        std::printf("real backend: a source switch beside the hung abort took %.0f ms\n", swap.ms);
        CHECK(swap.finished);
        CHECK(swap.ms < 500.0);
        {
            std::lock_guard<std::mutex> lk(fakepa::gateM);
            fakepa::released = true;
        }
        fakepa::gateCv.notify_all();
        if (swap.fut.valid()) { swap.fut.get(); }
    }
    // Release the hang, and everything that waited behind it finishes.
    {
        std::lock_guard<std::mutex> lk(fakepa::gateM);
        fakepa::released = true;
    }
    fakepa::gateCv.notify_all();
    if (destroyA.fut.valid()) { destroyA.fut.get(); }
    if (list.fut.valid()) { list.fut.get(); }
    if (openC.fut.valid()) { openC.fut.get(); }
    c.reset();
    // Every backend's initialisation is returned once its close has: the
    // hung one's too, now that it has finished.
    const auto t0 = Clock::now();
    while ((fakepa::hung.load() != 0 || fakepa::inits.load() != initsBefore) &&
           Clock::now() - t0 < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fakepa::hung.load() == 0);
    CHECK(fakepa::inits.load() == initsBefore);
    fakepa::hangOn = nullptr;
}

// Item 8: every Pa_Initialize and Pa_Terminate in the product goes through the
// one locked helper (PortAudio's own count is a plain int), and the helper
// keeps the count exact under contention.
void testPortAudioInitShared() {
    // The source, read: no other call site.
    namespace fs = std::filesystem;
    int offenders = 0;
    int sites = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(CASCADE_SOURCE_DIR) / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        if (e.path().filename() == "lang_assets.hpp") { continue; }
        std::FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (f == nullptr) { continue; }
        std::string text;
        char buf[65536];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
        std::fclose(f);
        for (const char* call : {"Pa_Initialize()", "Pa_Terminate()"}) {
            for (std::size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1)) {
                // A call, not a mention in a comment: the line up to it has no "//".
                const std::size_t bol = text.rfind('\n', at);
                const std::string head = text.substr(bol == std::string::npos ? 0 : bol + 1,
                                                     at - (bol == std::string::npos ? 0 : bol + 1));
                if (head.find("//") != std::string::npos) { continue; }
                ++sites;
                if (e.path().filename() != "pa_init.cpp") {
                    ++offenders;
                    std::printf("      %s calls %s outside sink/pa_init.cpp\n", e.path().string().c_str(), call);
                }
            }
        }
    }
    CHECK(sites == 2);  // the helper's own two
    CHECK(offenders == 0);

    // Under contention: with one initialisation held, eight threads pair
    // thousands of initialise/terminate calls; afterwards releasing the held
    // one must leave PortAudio uninitialised - the count came back exact.
    if (!cascade::sink::paInitializeShared()) {
        std::printf("      PortAudio would not initialise here; contention half skipped\n");
        return;
    }
    std::vector<std::thread> ts;
    std::atomic<int> failed{0};
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&failed] {
            for (int i = 0; i < 20000; ++i) {
                if (!cascade::sink::paInitializeShared()) {
                    ++failed;
                    continue;
                }
                cascade::sink::paTerminateShared();
            }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(failed.load() == 0);
    CHECK(Pa_GetDeviceCount() >= 0);  // still initialised: ours is held
    cascade::sink::paTerminateShared();
    const PaDeviceIndex after = Pa_GetDeviceCount();
    std::printf("pa init: after 160000 contended pairs, released: Pa_GetDeviceCount() = %d (%s)\n",
                static_cast<int>(after), after == paNotInitialized ? "uninitialised, count exact" : "COUNT DRIFTED");
    CHECK(after == paNotInitialized);
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
    // The review of 6b5aead, item by item.
    testFixedCentreEdge();
    testVfoInsideSpan();
    testWholeFramesAndOverruns();
    testStartDrainsStale();
    testRateChange();
    testAlivePollInterval();
    testConfigRateBounds();
    testRecoveryAdvice();
    testPatchLoanAndCentreBox();
    testHungCloseAtTheSource();
    testHostApiFilter();
    testRealBackendHungAbort();
    // Before anything else in this process initialises the real PortAudio.
    testPortAudioInitShared();
    testRealBackendEnumerates();
    return testSummary("test_soundcard_source");
}
